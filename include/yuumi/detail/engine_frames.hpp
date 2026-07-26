#pragma once

#include <yuumi/detail/engine_handshake.hpp>

namespace yuumi::detail {

inline bool EngineImpl::process_frame(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    std::uint8_t flags,
    std::span<const std::byte> payload
) {
    if ((flags & 0xF8U) != 0 ||
        ((flags & Protocol::LastFragment) != 0 && (flags & Protocol::Fragment) == 0)) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "invalid frame flags");
        return false;
    }
    if ((flags & Protocol::Correlated) != 0 &&
        (session->capabilities & CAP_CORRELATION) == 0) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "correlation was not negotiated");
        return false;
    }
    if (channel == Channel::Control) {
        if (flags != Protocol::None) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "Control frames cannot carry application prefixes");
            return false;
        }
        return process_control(session, payload);
    }
    if (channel != Channel::Command && channel != Channel::Data) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "unknown or direction-invalid input channel");
        return false;
    }
    return process_application(session, channel, flags, payload);
}

inline bool EngineImpl::process_control(
    const std::shared_ptr<EngineSession>& session,
    std::span<const std::byte> payload
) {
    auto decoded = Protocol::decode_control(payload);
    if (!decoded || !decoded->is_object() || !decoded->contains("type") || !(*decoded)["type"].is_string()) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "malformed Control object");
        return false;
    }
    const auto type = (*decoded)["type"].get<std::string>();
    if (type == "heartbeat" || type == "pong") {
        return true;
    }
    if (type == "ping") {
        if (!decoded->contains("seq") || !(*decoded)["seq"].is_number_unsigned()) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "ping requires an unsigned sequence");
            return false;
        }
        if (auto pong = write_control(session, Json{{"type", "pong"}, {"seq", (*decoded)["seq"]}}); !pong) {
            report_terminal(session, pong.error());
            request_close(session, DisconnectReason::TransportFailure);
            return false;
        }
        return true;
    }
    if (type == "error") {
        if (!decoded->contains("code") || !(*decoded)["code"].is_number_unsigned()) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "error Control requires an unsigned code");
            return false;
        }
        const auto code = (*decoded)["code"].get<std::uint32_t>();
        report_terminal(session, Protocol::error(
            ErrorCategory::Protocol,
            static_cast<StatusCode>(code),
            ErrorPhase::FrameDecode,
            decoded->value("message", std::string("peer reported a protocol error")),
            session->handle
        ));
        request_close(session, DisconnectReason::ProtocolFailure);
        return false;
    }
    return true;
}

inline bool EngineImpl::process_application(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    std::uint8_t flags,
    std::span<const std::byte> payload
) {
    const auto fragmented = (flags & Protocol::Fragment) != 0;
    const auto correlated = (flags & Protocol::Correlated) != 0;
    std::size_t prefix_size = fragmented ? 4U : 0U;
    prefix_size += correlated ? 4U : 0U;
    if (payload.size() < prefix_size) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, "frame payload is shorter than its active prefixes");
        return false;
    }
    std::size_t offset{};
    std::uint32_t fragment_id{};
    std::optional<std::uint32_t> correlation_id;
    if (fragmented) {
        fragment_id = Protocol::read_u32(std::span<const std::byte, 4>(payload.data(), 4));
        offset += 4;
    }
    if (correlated) {
        correlation_id = Protocol::read_u32(std::span<const std::byte, 4>(payload.data() + offset, 4));
        offset += 4;
    }
    if (!fragmented) {
        return decode_and_emit(session, channel, correlation_id, payload.subspan(offset));
    }
    const auto key = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(channel)) << 32U) | fragment_id;
    std::vector<std::byte> complete;
    std::optional<std::pair<StatusCode, std::string>> failure;
    {
        std::lock_guard fragment_lock(session->fragment_mutex);
        auto iterator = session->fragments.find(key);
        if (iterator == session->fragments.end()) {
            const auto interleaved = std::ranges::any_of(session->fragments, [channel](const auto& entry) {
                return entry.second.channel == channel;
            });
            if (interleaved || session->fragments.size() >= config.fragmentation.active_sequence_limit) {
                failure = {StatusCode::ERR_PROTOCOL_VIOLATION, "fragment sequence interleaves or exceeds the active limit"};
            } else {
                FragmentState state;
                state.channel = channel;
                state.fragment_id = fragment_id;
                state.correlated = correlated;
                state.correlation_id = correlation_id;
                state.deadline = std::chrono::steady_clock::now() + config.fragmentation.timeout;
                iterator = session->fragments.emplace(key, std::move(state)).first;
            }
        } else if (iterator->second.correlated != correlated ||
                   iterator->second.correlation_id != correlation_id) {
            session->fragments.erase(iterator);
            failure = {StatusCode::ERR_PROTOCOL_VIOLATION, "fragment prefix changed inside an active sequence"};
        }
        if (!failure) {
            const auto data = payload.subspan(offset);
            if (iterator->second.data.size() > MAX_MESSAGE_SIZE - data.size()) {
                session->fragments.erase(iterator);
                failure = {StatusCode::ERR_PAYLOAD_TOO_LARGE, "reassembled payload exceeds 16 MiB"};
            } else {
                iterator->second.data.insert(iterator->second.data.end(), data.begin(), data.end());
                if ((flags & Protocol::LastFragment) != 0) {
                    complete = std::move(iterator->second.data);
                    session->fragments.erase(iterator);
                }
            }
        }
    }
    if (failure) {
        protocol_failure(session, failure->first, std::move(failure->second));
        return false;
    }
    if ((flags & Protocol::LastFragment) == 0) {
        return true;
    }
    return decode_and_emit(session, channel, correlation_id, complete);
}

inline bool EngineImpl::decode_and_emit(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    std::optional<std::uint32_t> correlation_id,
    std::span<const std::byte> payload
) {
    auto decoded = Protocol::decode_payload(payload, session->encoding);
    if (!decoded) {
        protocol_failure(
            session,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            "application payload cannot be decoded with the negotiated encoding"
        );
        return false;
    }
    emit_message(session, MessageEvent{session->handle, channel, std::move(*decoded), correlation_id});
    return true;
}

inline void EngineImpl::protocol_failure(
    const std::shared_ptr<EngineSession>& session,
    StatusCode status,
    std::string cause
) {
    auto failure = Protocol::error(
        ErrorCategory::Protocol,
        status,
        ErrorPhase::FrameDecode,
        cause,
        session->handle
    );
    report_terminal(session, failure);
    static_cast<void>(write_control(session, Json{
        {"type", "error"},
        {"code", static_cast<std::uint32_t>(status)},
        {"message", std::move(cause)}
    }));
    request_close(session, DisconnectReason::ProtocolFailure);
}

inline Result<> EngineImpl::write_control(
    const std::shared_ptr<EngineSession>& session,
    const Json& value
) {
    auto frame = Protocol::control_frame(value);
    if (!frame) {
        auto failure = frame.error();
        failure.session = session->handle;
        return unexpected(std::move(failure));
    }
    return write_packet(session, *frame, ErrorPhase::FrameWrite);
}

inline Result<> EngineImpl::write_packet(
    const std::shared_ptr<EngineSession>& session,
    std::span<const std::byte> packet,
    ErrorPhase phase
) {
    std::lock_guard send_lock(session->send_mutex);
    if (session->close_requested.load(std::memory_order_acquire)) {
        return unexpected(Protocol::error(
            ErrorCategory::Session,
            StatusCode::ERR_CONNECTION_LOST,
            phase,
            "session is closing",
            session->handle
        ));
    }
    const auto written = session->stream->write_exact(packet);
    if (written.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ErrorCategory::Transport,
            StatusCode::ERR_WRITE_FAILED,
            phase,
            written.cause,
            session->handle
        ));
    }
    return {};
}

inline void EngineImpl::request_close(
    const std::shared_ptr<EngineSession>& session,
    DisconnectReason reason
) {
    bool expected = false;
    if (session->close_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        session->close_reason.store(reason, std::memory_order_release);
        session->stream->close();
    }
}

inline void EngineImpl::report_terminal(
    const std::shared_ptr<EngineSession>& session,
    ErrorInfo failure
) {
    bool expected = false;
    if (session->terminal_error_reported.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        emit_session_error(session, std::move(failure));
    }
}

inline std::shared_ptr<EngineSession> EngineImpl::live_session(const SessionHandle& handle) {
    std::lock_guard sessions_lock(sessions_mutex);
    const auto iterator = sessions.find(handle.session_id);
    if (iterator == sessions.end() || iterator->second->handle.epoch != handle.epoch ||
        iterator->second->close_requested.load(std::memory_order_acquire)) {
        return {};
    }
    return iterator->second;
}

inline Result<> EngineImpl::send(
    const SessionHandle& handle,
    Channel channel,
    const Json& payload,
    std::optional<std::uint32_t> correlation_id
) {
    if (channel != Channel::Log && channel != Channel::Data) {
        return unexpected(Protocol::error(
            ErrorCategory::Session,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::ApplicationSend,
            "engine applications may send only Log or Data",
            handle
        ));
    }
    auto session = live_session(handle);
    if (!session) {
        return unexpected(Protocol::error(
            ErrorCategory::Session,
            StatusCode::ERR_CONNECTION_LOST,
            ErrorPhase::ApplicationSend,
            "session handle is absent, closed, or stale",
            handle
        ));
    }
    if (correlation_id && (session->capabilities & CAP_CORRELATION) == 0) {
        return unexpected(Protocol::error(
            ErrorCategory::Session,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::ApplicationSend,
            "correlation was not negotiated",
            handle
        ));
    }
    auto encoded = Protocol::encode_payload(payload, session->encoding);
    if (!encoded) {
        auto failure = encoded.error();
        failure.session = handle;
        return unexpected(std::move(failure));
    }
    std::vector<std::byte> framed_payload;
    framed_payload.reserve(encoded->size() + (correlation_id ? 4U : 0U));
    if (correlation_id) {
        Protocol::append_u32(framed_payload, *correlation_id);
    }
    if (encoded->size() > MAX_MESSAGE_SIZE - framed_payload.size()) {
        return unexpected(Protocol::error(
            ErrorCategory::Serialization,
            StatusCode::ERR_PAYLOAD_TOO_LARGE,
            ErrorPhase::ApplicationSend,
            "encoded frame exceeds 16 MiB",
            handle
        ));
    }
    framed_payload.insert(framed_payload.end(), encoded->begin(), encoded->end());
    auto packet = Protocol::frame(
        channel,
        correlation_id ? Protocol::Correlated : Protocol::None,
        framed_payload
    );
    if (!packet) {
        auto failure = packet.error();
        failure.session = handle;
        return unexpected(std::move(failure));
    }
    auto written = write_packet(session, *packet, ErrorPhase::FrameWrite);
    if (!written) {
        report_terminal(session, written.error());
        request_close(session, DisconnectReason::TransportFailure);
    }
    return written;
}

}

/*
 * Frame processing validates size, direction, flags, prefix order, negotiated
 * correlation, and per-session fragment consistency before decoding. Fatal
 * protocol errors emit one JSON Control error and close only that session.
 */
