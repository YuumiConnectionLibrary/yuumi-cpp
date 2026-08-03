#pragma once

#include <yuumi/detail/engine_lifecycle.hpp>

namespace yuumi::detail {

inline Result<SessionView> EngineImpl::establish(
    const std::shared_ptr<Stream>& stream,
    const EngineConfig& config,
    Deadline deadline
) {
    std::array<std::byte, 16> handshake{};
    const auto read = stream->read_exact(handshake, deadline);
    if (read.state != IoState::Complete) {
        return unexpected(Protocol::error(
            read.state == IoState::TimedOut ? ErrorKind::Timeout : ErrorKind::Handshake,
            read.cause,
            read.state == IoState::TimedOut
                ? StatusCode::ERR_READ_TIMEOUT
                : StatusCode::ERR_CONNECTION_LOST,
            ErrorPhase::HandshakeRead
        ));
    }
    const auto magic = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data(), 4));
    const auto version = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data() + 4, 4));
    const auto process_id = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data() + 8, 4));
    if (magic != 0x59554D49) {
        return unexpected(Protocol::error(
            ErrorKind::Handshake,
            "handshake magic is invalid",
            StatusCode::ERR_MAGIC_MISMATCH,
            ErrorPhase::HandshakeValidate
        ));
    }
    if (version != PROTOCOL_VERSION) {
        return unexpected(Protocol::error(
            ErrorKind::Handshake,
            "handshake protocol version is incompatible",
            StatusCode::ERR_VERSION_MISMATCH,
            ErrorPhase::HandshakeValidate
        ));
    }
    const auto authenticated_pid = stream->peer_pid();
    if ((config.expected_go_pid && process_id != *config.expected_go_pid) ||
        (authenticated_pid && process_id != *authenticated_pid)) {
        return unexpected(Protocol::error(
            ErrorKind::Handshake,
            "Go PID does not match expected or authenticated peer",
            StatusCode::ERR_PID_MISMATCH,
            ErrorPhase::HandshakeValidate
        ));
    }
    const auto encoding_mask = std::to_integer<std::uint8_t>(handshake[12]);
    std::optional<Encoding> encoding;
    for (const auto candidate : config.supported_encodings) {
        if ((encoding_mask & static_cast<std::uint8_t>(candidate)) != 0) {
            encoding = candidate;
            break;
        }
    }
    if (!encoding) {
        return unexpected(Protocol::error(
            ErrorKind::Encoding,
            "no common encoding exists",
            StatusCode::ERR_ENCODING_UNSUPPORTED,
            ErrorPhase::HandshakeValidate
        ));
    }
    const auto peer_capabilities =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[13])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[14])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[15]));
    const auto capabilities =
        peer_capabilities & config.supported_capabilities & IMPLEMENTED_CAPABILITIES;
    const std::array<std::byte, 4> acknowledgement{
        static_cast<std::byte>(*encoding),
        static_cast<std::byte>((capabilities >> 16U) & 0xFFU),
        static_cast<std::byte>((capabilities >> 8U) & 0xFFU),
        static_cast<std::byte>(capabilities & 0xFFU)
    };
    if (test_hooks && test_hooks->fail_ack_write.exchange(false, std::memory_order_acq_rel)) {
        return unexpected(Protocol::error(
            ErrorKind::Handshake,
            "injected ACK write failure",
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::AckWrite
        ));
    }
    const auto ack_write = stream->write_exact(acknowledgement, deadline);
    if (ack_write.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ack_write.state == IoState::TimedOut ? ErrorKind::Timeout : ErrorKind::Handshake,
            ack_write.cause,
            ack_write.state == IoState::TimedOut
                ? StatusCode::ERR_READ_TIMEOUT
                : StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::AckWrite
        ));
    }
    const auto session_id = session_identifier(next_session_id.fetch_add(1, std::memory_order_relaxed));
    auto assignment = Protocol::control_frame(Json{
        {"type", "session"},
        {"session_id", session_id}
    });
    if (!assignment) {
        auto failure = assignment.error();
        failure.kind = ErrorKind::Handshake;
        failure.phase = ErrorPhase::SessionWrite;
        return unexpected(std::move(failure));
    }
    if (test_hooks && test_hooks->fail_session_write.exchange(false, std::memory_order_acq_rel)) {
        return unexpected(Protocol::error(
            ErrorKind::Handshake,
            "injected session assignment write failure",
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::SessionWrite
        ));
    }
    const auto session_write = stream->write_exact(*assignment, deadline);
    if (session_write.state != IoState::Complete) {
        return unexpected(Protocol::error(
            session_write.state == IoState::TimedOut ? ErrorKind::Timeout : ErrorKind::Handshake,
            session_write.cause,
            session_write.state == IoState::TimedOut
                ? StatusCode::ERR_READ_TIMEOUT
                : StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::SessionWrite
        ));
    }
    return SessionView{
        session_id,
        next_epoch.load(std::memory_order_acquire) + 1,
        *encoding,
        capabilities
    };
}

inline void EngineImpl::reader_loop(const std::shared_ptr<EngineSession>& session) {
    while (!session->close_requested.load(std::memory_order_acquire)) {
        if (test_hooks && test_hooks->fail_frame_read.exchange(false, std::memory_order_acq_rel)) {
            set_terminal(session, DisconnectReason::TransportFailure, Protocol::error(
                ErrorKind::Transport,
                "injected established-session read failure",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::FrameRead,
                session->view.epoch
            ));
            request_close(session, DisconnectReason::TransportFailure);
            break;
        }
        std::array<std::byte, 6> header{};
        const auto header_read = session->stream->read_exact(header);
        if (header_read.state != IoState::Complete) {
            if (!session->close_requested.load(std::memory_order_acquire)) {
                if (header_read.state == IoState::Closed) {
                    request_close(session, DisconnectReason::PeerClose);
                } else {
                    set_terminal(session, DisconnectReason::TransportFailure, Protocol::error(
                        ErrorKind::Transport,
                        header_read.cause,
                        StatusCode::ERR_CONNECTION_LOST,
                        ErrorPhase::FrameRead,
                        session->view.epoch
                    ));
                    request_close(session, DisconnectReason::TransportFailure);
                }
            }
            break;
        }
        const auto payload_size = Protocol::read_u32(std::span<const std::byte, 4>(header.data(), 4));
        if (payload_size > MAX_MESSAGE_SIZE) {
            protocol_failure(
                session,
                StatusCode::ERR_PAYLOAD_TOO_LARGE,
                ErrorPhase::FrameDecode,
                "frame payload exceeds 16 MiB"
            );
            break;
        }
        if (test_hooks) {
            test_hooks->payload_read_attempts.fetch_add(1, std::memory_order_relaxed);
            auto observed = test_hooks->largest_payload_allocation.load(std::memory_order_relaxed);
            while (observed < payload_size &&
                   !test_hooks->largest_payload_allocation.compare_exchange_weak(
                       observed,
                       payload_size,
                       std::memory_order_relaxed)) {}
        }
        std::vector<std::byte> payload(payload_size);
        const auto payload_read = session->stream->read_exact(payload);
        if (payload_read.state != IoState::Complete) {
            set_terminal(session, DisconnectReason::TransportFailure, Protocol::error(
                ErrorKind::Transport,
                payload_read.cause,
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::FrameRead,
                session->view.epoch
            ));
            request_close(session, DisconnectReason::TransportFailure);
            break;
        }
        const auto channel = static_cast<Channel>(std::to_integer<std::uint8_t>(header[4]));
        const auto flags = std::to_integer<std::uint8_t>(header[5]);
        if (!process_frame(session, channel, flags, payload)) {
            break;
        }
        session->last_activity.store(ticks(std::chrono::steady_clock::now()), std::memory_order_release);
    }
    session->stream->close();
    finalize_session(session);
}

}

/*
 * Establishment reads exactly one handshake before parsing, writes ACK before
 * assignment, and creates an epoch only after both writes complete. Invalid
 * candidates never enter connected state or emit lifecycle events.
 */
