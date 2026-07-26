#pragma once

#include <yuumi/detail/engine_lifecycle.hpp>

namespace yuumi::detail {

inline Result<> EngineImpl::establish(const std::shared_ptr<EngineSession>& session) {
    std::array<std::byte, 16> handshake{};
    const auto read = session->stream->read_exact(handshake);
    if (read.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ErrorCategory::Handshake,
            read.state == IoState::Closed ? StatusCode::ERR_CONNECTION_LOST : StatusCode::ERR_READ_TIMEOUT,
            ErrorPhase::HandshakeRead,
            read.cause
        ));
    }
    const auto magic = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data(), 4));
    const auto version = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data() + 4, 4));
    const auto process_id = Protocol::read_u32(std::span<const std::byte, 4>(handshake.data() + 8, 4));
    if (magic != 0x59554D49) {
        return unexpected(Protocol::error(
            ErrorCategory::Handshake,
            StatusCode::ERR_MAGIC_MISMATCH,
            ErrorPhase::HandshakeValidate,
            "handshake magic does not match"
        ));
    }
    if (version != PROTOCOL_VERSION) {
        return unexpected(Protocol::error(
            ErrorCategory::Handshake,
            StatusCode::ERR_VERSION_MISMATCH,
            ErrorPhase::HandshakeValidate,
            "handshake protocol version is incompatible"
        ));
    }
    const auto authenticated_pid = session->stream->peer_pid();
    if (config.expected_pid &&
        (process_id != *config.expected_pid ||
         (authenticated_pid && process_id != *authenticated_pid))) {
        return unexpected(Protocol::error(
            ErrorCategory::Handshake,
            StatusCode::ERR_PID_MISMATCH,
            ErrorPhase::HandshakeValidate,
            "handshake PID does not match the configured or authenticated peer"
        ));
    }
    const auto encoding_mask = std::to_integer<std::uint8_t>(handshake[12]);
    bool selected{};
    for (const auto encoding : config.supported_encodings) {
        if ((encoding_mask & static_cast<std::uint8_t>(encoding)) != 0) {
            session->encoding = encoding;
            selected = true;
            break;
        }
    }
    if (!selected) {
        return unexpected(Protocol::error(
            ErrorCategory::Handshake,
            StatusCode::ERR_ENCODING_UNSUPPORTED,
            ErrorPhase::HandshakeValidate,
            "handshake has no supported encoding intersection"
        ));
    }
    const auto client_capabilities =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[13])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[14])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(handshake[15]));
    session->capabilities = client_capabilities & config.supported_capabilities & IMPLEMENTED_CAPABILITIES;
    const std::array<std::byte, 4> acknowledgement{
        static_cast<std::byte>(session->encoding),
        static_cast<std::byte>((session->capabilities >> 16U) & 0xFFU),
        static_cast<std::byte>((session->capabilities >> 8U) & 0xFFU),
        static_cast<std::byte>(session->capabilities & 0xFFU)
    };
    if (test_hooks && test_hooks->fail_ack_write.exchange(false, std::memory_order_acq_rel)) {
        return unexpected(Protocol::error(
            ErrorCategory::Transport,
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::AckWrite,
            "injected ACK write failure"
        ));
    }
    const auto ack_write = session->stream->write_exact(acknowledgement);
    if (ack_write.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ErrorCategory::Transport,
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::AckWrite,
            ack_write.cause
        ));
    }
    session->handle.session_id = session_identifier(next_session_id.fetch_add(1, std::memory_order_relaxed));
    auto assignment = Protocol::control_frame(Json{
        {"type", "session"},
        {"session_id", session->handle.session_id}
    });
    if (!assignment) {
        auto failure = assignment.error();
        failure.phase = ErrorPhase::SessionWrite;
        return unexpected(std::move(failure));
    }
    if (test_hooks && test_hooks->fail_session_write.exchange(false, std::memory_order_acq_rel)) {
        return unexpected(Protocol::error(
            ErrorCategory::Transport,
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::SessionWrite,
            "injected session write failure"
        ));
    }
    const auto session_write = session->stream->write_exact(*assignment);
    if (session_write.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ErrorCategory::Transport,
            StatusCode::ERR_WRITE_FAILED,
            ErrorPhase::SessionWrite,
            session_write.cause
        ));
    }
    session->handle.epoch = next_epoch.fetch_add(1, std::memory_order_relaxed);
    const auto now = ticks(std::chrono::steady_clock::now());
    session->last_activity.store(now, std::memory_order_release);
    session->last_heartbeat.store(now, std::memory_order_release);
    {
        std::lock_guard sessions_lock(sessions_mutex);
        sessions.emplace(session->handle.session_id, session);
    }
    session->established.store(true, std::memory_order_release);
    emit_connected(session);
    return {};
}

inline void EngineImpl::session_loop(const std::shared_ptr<EngineSession>& session) {
    auto established = establish(session);
    if (!established) {
        emit_error(established.error());
        session->stream->close();
        release_session(session);
        return;
    }
    while (!session->close_requested.load(std::memory_order_acquire)) {
        if (test_hooks && test_hooks->fail_frame_read.exchange(false, std::memory_order_acq_rel)) {
            report_terminal(session, Protocol::error(
                ErrorCategory::Transport,
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::FrameRead,
                "injected established-session read failure",
                session->handle
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
                    report_terminal(session, Protocol::error(
                        ErrorCategory::Transport,
                        StatusCode::ERR_CONNECTION_LOST,
                        ErrorPhase::FrameRead,
                        header_read.cause,
                        session->handle
                    ));
                    request_close(session, DisconnectReason::TransportFailure);
                }
            }
            break;
        }
        const auto payload_size = Protocol::read_u32(std::span<const std::byte, 4>(header.data(), 4));
        if (payload_size > MAX_MESSAGE_SIZE) {
            protocol_failure(session, StatusCode::ERR_PAYLOAD_TOO_LARGE, "payload exceeds 16 MiB");
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
            report_terminal(session, Protocol::error(
                ErrorCategory::Transport,
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::FrameRead,
                payload_read.cause,
                session->handle
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
    if (session->established.load(std::memory_order_acquire)) {
        emit_disconnected(session);
    }
    release_session(session);
}

inline void EngineImpl::release_session(const std::shared_ptr<EngineSession>& session) {
    std::lock_guard sessions_lock(sessions_mutex);
    if (!session->handle.session_id.empty()) {
        const auto iterator = sessions.find(session->handle.session_id);
        if (iterator != sessions.end() && iterator->second == session) {
            sessions.erase(iterator);
        }
    }
    connections.erase(session);
}

}

/*
 * A connection remains pre-session until its exact handshake, ACK, and session
 * assignment writes succeed. Capacity is released after teardown, while epoch
 * allocation occurs only for a fully established visible session.
 */
