#pragma once

#include <yuumi/detail/engine_handshake.hpp>

namespace yuumi::detail {

inline bool EngineImpl::process_frame(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    std::uint8_t flags,
    std::span<const std::byte> payload
) {
    if (channel != Channel::Control && channel != Channel::Command &&
        channel != Channel::Log && channel != Channel::Data) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "frame channel is unknown");
        return false;
    }
    if ((flags & 0xF8U) != 0 ||
        ((flags & Protocol::LastFragment) != 0 && (flags & Protocol::Fragment) == 0)) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "frame flags are invalid");
        return false;
    }
    if (channel == Channel::Log) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "Log is not valid Go-to-engine traffic");
        return false;
    }
    if (channel == Channel::Control) {
        if (flags != Protocol::None) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "Control frames cannot carry application flags");
            return false;
        }
        return process_control(session, payload);
    }
    if ((flags & Protocol::Correlated) != 0 &&
        (session->view.capabilities & CAP_CORRELATION) == 0) {
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "correlation was not negotiated");
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
        protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "malformed Control object");
        return false;
    }
    const auto type = (*decoded)["type"].get<std::string>();
    if (type == "heartbeat") {
        const auto timestamp = decoded->contains("ts") && (*decoded)["ts"].is_number_integer()
            ? (*decoded)["ts"].get<std::int64_t>()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        return enqueue_application(session, DispatchEvent{
            DispatchEvent::Type::Heartbeat,
            HeartbeatEvent{session->view, timestamp},
            true
        });
    }
    if (type == "pong") {
        return true;
    }
    if (type == "ping") {
        if (!decoded->contains("seq") || !(*decoded)["seq"].is_number_unsigned()) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "ping requires an unsigned sequence");
            return false;
        }
        if (auto pong = write_control(session, Json{{"type", "pong"}, {"seq", (*decoded)["seq"]}}); !pong) {
            set_terminal(session, DisconnectReason::TransportFailure, pong.error());
            request_close(session, DisconnectReason::TransportFailure);
            return false;
        }
        return true;
    }
    if (type == "error") {
        if (!decoded->contains("code") || !(*decoded)["code"].is_number_unsigned()) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "Control error requires an unsigned code");
            return false;
        }
        const auto code = (*decoded)["code"].get<std::uint32_t>();
        set_terminal(session, DisconnectReason::ProtocolFailure, Protocol::error(
            ErrorKind::Protocol,
            decoded->value("message", std::string("Go peer reported a protocol error")),
            static_cast<StatusCode>(code),
            ErrorPhase::FrameDecode,
            session->view.epoch
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
    std::size_t offset{};
    std::uint32_t fragment_id{};
    std::optional<std::uint32_t> correlation_id;
    if (fragmented) {
        if (payload.size() < 4) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "fragment prefix is shorter than four bytes");
            return false;
        }
        fragment_id = Protocol::read_u32(std::span<const std::byte, 4>(payload.data(), 4));
        offset += 4;
    }
    if (correlated) {
        if (payload.size() - offset < 4) {
            protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::FrameDecode, "correlation prefix is shorter than four bytes");
            return false;
        }
        correlation_id = Protocol::read_u32(std::span<const std::byte, 4>(payload.data() + offset, 4));
        offset += 4;
    }
    if (!fragmented) {
        {
            std::lock_guard lock(session->fragment_mutex);
            if (std::ranges::any_of(session->fragments, [channel](const auto& item) {
                    return item.second.channel == channel;
                })) {
                protocol_failure(session, StatusCode::ERR_PROTOCOL_VIOLATION, ErrorPhase::Fragmentation, "fragment sequences cannot be interleaved on one channel");
                return false;
            }
        }
        return decode_and_enqueue(session, channel, correlation_id, payload.subspan(offset));
    }

    const auto key = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(channel)) << 32U) | fragment_id;
    std::vector<std::byte> complete;
    std::optional<std::pair<StatusCode, std::string>> failure;
    {
        std::lock_guard lock(session->fragment_mutex);
        auto iterator = session->fragments.find(key);
        if (iterator == session->fragments.end()) {
            const auto interleaved = std::ranges::any_of(session->fragments, [channel](const auto& item) {
                return item.second.channel == channel;
            });
            if (interleaved || session->fragments.size() >= session->config.fragmentation.active_sequence_limit) {
                failure = {StatusCode::ERR_PROTOCOL_VIOLATION, "fragment sequence interleaves or exceeds the active limit"};
            } else {
                FragmentState state;
                state.channel = channel;
                state.fragment_id = fragment_id;
                state.correlation_id = correlation_id;
                state.deadline = std::chrono::steady_clock::now() + session->config.fragmentation.timeout;
                iterator = session->fragments.emplace(key, std::move(state)).first;
            }
        } else if (iterator->second.correlation_id != correlation_id) {
            session->fragments.erase(iterator);
            failure = {StatusCode::ERR_PROTOCOL_VIOLATION, "fragment sequence prefixes changed or interleaved"};
        }
        if (!failure) {
            const auto data = payload.subspan(offset);
            if (iterator->second.data.size() > MAX_MESSAGE_SIZE - data.size()) {
                session->fragments.erase(iterator);
                failure = {StatusCode::ERR_PAYLOAD_TOO_LARGE, "reassembled message exceeds 16 MiB"};
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
        protocol_failure(session, failure->first, ErrorPhase::Fragmentation, std::move(failure->second));
        return false;
    }
    if ((flags & Protocol::LastFragment) == 0) {
        return true;
    }
    return decode_and_enqueue(session, channel, correlation_id, complete);
}

inline bool EngineImpl::decode_and_enqueue(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    std::optional<std::uint32_t> correlation_id,
    std::span<const std::byte> payload
) {
    auto decoded = Protocol::decode_payload(payload, session->view.encoding);
    if (!decoded) {
        protocol_failure(
            session,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::FrameDecode,
            "application payload cannot be decoded with the negotiated encoding"
        );
        return false;
    }
    std::shared_ptr<Responder> responder;
    if (correlation_id) {
        auto responder_state = std::make_shared<Responder::State>();
        responder_state->epoch = session->view.epoch;
        responder_state->correlation_id = *correlation_id;
        const auto weak = weak_from_this();
        responder_state->send = [weak, epoch = session->view.epoch, identifier = *correlation_id](const Json& response) {
            if (const auto self = weak.lock()) {
                return self->respond(epoch, identifier, response);
            }
            return Result<>(unexpected(Protocol::error(
                ErrorKind::StaleEpoch,
                "responder owner no longer exists",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::ApplicationSend,
                epoch
            )));
        };
        responder = std::shared_ptr<Responder>(new Responder(std::move(responder_state)));
        std::lock_guard lock(session->responder_mutex);
        session->responders.emplace_back(responder);
    }
    return enqueue_application(session, DispatchEvent{
        DispatchEvent::Type::Message,
        MessageEvent{session->view, channel, std::move(*decoded), correlation_id, responder},
        true
    });
}

inline void EngineImpl::protocol_failure(
    const std::shared_ptr<EngineSession>& session,
    StatusCode status,
    ErrorPhase phase,
    std::string cause
) {
    set_terminal(session, DisconnectReason::ProtocolFailure, Protocol::error(
        ErrorKind::Protocol,
        cause,
        status,
        phase,
        session->view.epoch
    ));
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
        failure.epoch = session->view.epoch;
        return unexpected(std::move(failure));
    }
    return write_packet(session, *frame, ErrorPhase::FrameWrite);
}

inline Result<> EngineImpl::write_packet(
    const std::shared_ptr<EngineSession>& session,
    std::span<const std::byte> packet,
    ErrorPhase phase
) {
    if (session->close_requested.load(std::memory_order_acquire)) {
        return unexpected(Protocol::error(
            ErrorKind::SessionClosed,
            "session is closing",
            StatusCode::ERR_CONNECTION_LOST,
            phase,
            session->view.epoch
        ));
    }
    const auto written = session->stream->write_exact(packet);
    if (written.state != IoState::Complete) {
        return unexpected(Protocol::error(
            ErrorKind::Transport,
            written.cause,
            StatusCode::ERR_WRITE_FAILED,
            phase,
            session->view.epoch
        ));
    }
    return {};
}

inline Result<> EngineImpl::send(Channel channel, const Json& payload) {
    if (channel != Channel::Log && channel != Channel::Data) {
        return unexpected(Protocol::error(
            ErrorKind::Protocol,
            "engine applications may send only Log or Data",
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::ApplicationSend,
            session().transform([](const SessionView& value) { return value.epoch; })
        ));
    }
    std::shared_ptr<EngineSession> active;
    {
        std::lock_guard lock(lifecycle_mutex);
        if (lifecycle != EngineState::Connected || !current ||
            current->close_requested.load(std::memory_order_acquire)) {
            return unexpected(Protocol::error(
                ErrorKind::SessionClosed,
                "engine has no live session",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::ApplicationSend
            ));
        }
        active = current;
    }
    return send_packet(active, channel, payload, std::nullopt);
}

inline Result<> EngineImpl::respond(
    std::uint64_t epoch,
    std::uint32_t correlation_id,
    const Json& payload
) {
    std::shared_ptr<EngineSession> active;
    {
        std::lock_guard lock(lifecycle_mutex);
        if (lifecycle != EngineState::Connected || !current || current->view.epoch != epoch ||
            current->close_requested.load(std::memory_order_acquire)) {
            return unexpected(Protocol::error(
                ErrorKind::StaleEpoch,
                "responder belongs to an earlier or closed epoch",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::ApplicationSend,
                epoch
            ));
        }
        active = current;
    }
    if ((active->view.capabilities & CAP_CORRELATION) == 0) {
        return unexpected(Protocol::error(
            ErrorKind::Capability,
            "correlation was not negotiated",
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::ApplicationSend,
            epoch
        ));
    }
    return send_packet(active, Channel::Data, payload, correlation_id);
}

inline Result<> EngineImpl::send_packet(
    const std::shared_ptr<EngineSession>& session,
    Channel channel,
    const Json& payload,
    std::optional<std::uint32_t> correlation_id
) {
    auto encoded = Protocol::encode_payload(payload, session->view.encoding);
    if (!encoded) {
        auto failure = encoded.error();
        failure.kind = ErrorKind::Encoding;
        failure.phase = ErrorPhase::ApplicationSend;
        failure.epoch = session->view.epoch;
        return unexpected(std::move(failure));
    }
    std::vector<std::byte> framed_payload;
    framed_payload.reserve(encoded->size() + (correlation_id ? 4U : 0U));
    if (correlation_id) {
        Protocol::append_u32(framed_payload, *correlation_id);
    }
    if (encoded->size() > MAX_MESSAGE_SIZE - framed_payload.size()) {
        return unexpected(Protocol::error(
            ErrorKind::Protocol,
            "encoded frame exceeds 16 MiB",
            StatusCode::ERR_PAYLOAD_TOO_LARGE,
            ErrorPhase::ApplicationSend,
            session->view.epoch
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
        failure.epoch = session->view.epoch;
        return unexpected(std::move(failure));
    }
    auto written = write_packet(session, *packet, ErrorPhase::FrameWrite);
    if (!written) {
        set_terminal(session, DisconnectReason::TransportFailure, written.error());
        request_close(session, DisconnectReason::TransportFailure);
    }
    return written;
}

inline void EngineImpl::request_close(
    const std::shared_ptr<EngineSession>& session,
    DisconnectReason reason
) {
    bool expected = false;
    if (session->close_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        set_terminal(session, reason);
        session->accepting_events.store(false, std::memory_order_release);
        session->stream->close();
    }
}

inline void EngineImpl::set_terminal(
    const std::shared_ptr<EngineSession>& session,
    DisconnectReason reason,
    std::optional<ErrorInfo> failure
) {
    std::lock_guard lock(session->terminal_mutex);
    if (session->terminal.error || session->terminal.reason != DisconnectReason::PeerClose) {
        return;
    }
    session->terminal.reason = reason;
    session->terminal.error = std::move(failure);
}

}

/*
 * Frame processing is defensive before allocation and dispatch. Correlated
 * input carries a single-use epoch-bound Responder; public sends target only
 * the current session and preserve native write-mutex acquisition order.
 */
