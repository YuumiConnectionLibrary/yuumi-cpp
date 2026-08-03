#pragma once

#include <yuumi/detail/engine_state.hpp>

namespace yuumi::detail {

inline EngineImpl::~EngineImpl() {
    join_workers();
}

inline Result<> EngineImpl::validate_config(const EngineConfig& config) const {
    if (!valid_endpoint_name(config.endpoint_name)) {
        return unexpected(configuration_error("invalid endpoint_name"));
    }
    if (!valid_token(config.token)) {
        return unexpected(configuration_error("invalid token"));
    }
    if (config.supported_encodings.empty()) {
        return unexpected(configuration_error("supported_encodings must not be empty"));
    }
    std::uint8_t encodings{};
    for (const auto encoding : config.supported_encodings) {
        const auto bit = static_cast<std::uint8_t>(encoding);
        if (encoding != Encoding::JSON && encoding != Encoding::MsgPack) {
            return unexpected(configuration_error("supported_encodings contains an unknown value"));
        }
        if ((encodings & bit) != 0) {
            return unexpected(configuration_error("supported_encodings contains a duplicate"));
        }
        encodings |= bit;
    }
    if ((config.supported_capabilities & ~IMPLEMENTED_CAPABILITIES) != 0) {
        return unexpected(Protocol::error(
            ErrorKind::Capability,
            "supported_capabilities enables an unimplemented bit",
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration
        ));
    }
    if (config.connect_timeout.count() <= 0) {
        return unexpected(configuration_error("connect_timeout must be positive"));
    }
    if (config.application_queue_capacity == 0) {
        return unexpected(configuration_error("application_queue_capacity must be positive"));
    }
    if (!config.heartbeat.disabled &&
        (config.heartbeat.interval.count() <= 0 || config.heartbeat.missed_interval_limit == 0)) {
        return unexpected(configuration_error("enabled heartbeat values must be positive"));
    }
    if (config.fragmentation.timeout.count() <= 0 ||
        config.fragmentation.active_sequence_limit == 0) {
        return unexpected(configuration_error("fragmentation values must be positive"));
    }
    return {};
}

inline EngineState EngineImpl::state() const {
    std::lock_guard lock(lifecycle_mutex);
    return lifecycle;
}

inline std::optional<SessionView> EngineImpl::session() const {
    std::lock_guard lock(lifecycle_mutex);
    if (lifecycle != EngineState::Connected || !current) {
        return std::nullopt;
    }
    return current->view;
}

inline void EngineImpl::join_workers() {
    const auto own = std::this_thread::get_id();
    const auto settle = [own](std::thread& worker) {
        if (!worker.joinable()) {
            return;
        }
        if (worker.get_id() == own) {
            worker.detach();
        } else {
            worker.join();
        }
    };
    settle(reader_thread);
    settle(maintenance_thread);
    settle(dispatcher_thread);
}

inline Result<SessionView> EngineImpl::connect() {
    EngineConfig config;
    {
        std::lock_guard lock(lifecycle_mutex);
        if (lifecycle != EngineState::Idle) {
            return unexpected(Protocol::error(
                ErrorKind::State,
                lifecycle == EngineState::Connecting
                    ? "engine is already connecting"
                    : "engine is already connected or closing",
                StatusCode::ERR_PROTOCOL_VIOLATION,
                ErrorPhase::Dial
            ));
        }
        lifecycle = EngineState::Connecting;
        cancel_connect.store(false, std::memory_order_release);
        config = source_config;
    }
    join_workers();

    const auto fail = [this](ErrorInfo failure) -> Result<SessionView> {
        {
            std::lock_guard lock(lifecycle_mutex);
            candidate_stream.reset();
            lifecycle = EngineState::Idle;
        }
        lifecycle_cv.notify_all();
        emit_pre_session_error(failure);
        return unexpected(std::move(failure));
    };

    if (auto valid = validate_config(config); !valid) {
        return fail(valid.error());
    }
    auto address = resolve_transport_address(config.endpoint_name, config.token);
    if (!address) {
        return fail(address.error());
    }
    const auto deadline = std::chrono::steady_clock::now() + config.connect_timeout;
    if (test_hooks) {
        test_hooks->dial_attempts.fetch_add(1, std::memory_order_relaxed);
        if (test_hooks->fail_dial.exchange(false, std::memory_order_acq_rel)) {
            return fail(Protocol::error(
                ErrorKind::Dial,
                "injected dial failure",
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Dial
            ));
        }
    }
    auto connected = dial(*address, deadline, cancel_connect);
    if (!connected) {
        return fail(connected.error());
    }
    bool closed_during_dial{};
    {
        std::lock_guard lock(lifecycle_mutex);
        if (lifecycle != EngineState::Connecting || cancel_connect.load(std::memory_order_acquire)) {
            closed_during_dial = true;
        } else {
            candidate_stream = *connected;
        }
    }
    if (closed_during_dial) {
        (*connected)->close();
        return fail(Protocol::error(
            ErrorKind::SessionClosed,
            "connection attempt was closed locally",
            StatusCode::ERR_CONNECTION_LOST,
            ErrorPhase::Close
        ));
    }

    auto established = establish(*connected, config, deadline);
    if (!established) {
        (*connected)->close();
        if (cancel_connect.load(std::memory_order_acquire)) {
            return fail(Protocol::error(
                ErrorKind::SessionClosed,
                "connection attempt was closed locally",
                StatusCode::ERR_CONNECTION_LOST,
                ErrorPhase::Close
            ));
        }
        return fail(established.error());
    }

    auto active = std::make_shared<EngineSession>(*connected, *established, config);
    const auto now = ticks(std::chrono::steady_clock::now());
    active->last_activity.store(now, std::memory_order_release);
    active->last_heartbeat.store(now, std::memory_order_release);
    bool closed_during_establishment{};
    {
        std::lock_guard lock(lifecycle_mutex);
        if (lifecycle != EngineState::Connecting || cancel_connect.load(std::memory_order_acquire)) {
            closed_during_establishment = true;
        } else {
            current = active;
            candidate_stream.reset();
            next_epoch.store(active->view.epoch, std::memory_order_release);
            lifecycle = EngineState::Connected;
        }
    }
    if (closed_during_establishment) {
        active->stream->close();
        return fail(Protocol::error(
            ErrorKind::SessionClosed,
            "connection attempt was closed locally",
            StatusCode::ERR_CONNECTION_LOST,
            ErrorPhase::Close
        ));
    }
    lifecycle_cv.notify_all();

    try {
        auto self = shared_from_this();
        dispatcher_thread = std::thread([self, active] { self->dispatcher_loop(active); });
        enqueue_application(active, DispatchEvent{
            DispatchEvent::Type::Connected,
            active->view,
            true
        });
        reader_thread = std::thread([self, active] { self->reader_loop(active); });
        maintenance_thread = std::thread([self, active] { self->maintenance_loop(active); });
    } catch (const std::system_error& exception) {
        const auto failure = Protocol::error(
            ErrorKind::Internal,
            exception.what(),
            StatusCode::ERR_INTERNAL,
            ErrorPhase::Close,
            active->view.epoch
        );
        set_terminal(active, DisconnectReason::TransportFailure, failure);
        request_close(active, DisconnectReason::TransportFailure);
        finalize_session(active);
        join_workers();
        return unexpected(failure);
    }
    return active->view;
}

inline Result<> EngineImpl::close() {
    std::shared_ptr<EngineSession> active;
    {
        std::unique_lock lock(lifecycle_mutex);
        if (lifecycle == EngineState::Idle) {
            lock.unlock();
            join_workers();
            return {};
        }
        if (lifecycle == EngineState::Closing) {
            if (dispatcher_thread.joinable() && dispatcher_thread.get_id() == std::this_thread::get_id()) {
                return {};
            }
            lifecycle_cv.wait(lock, [this] { return lifecycle == EngineState::Idle; });
            lock.unlock();
            join_workers();
            return {};
        }
        lifecycle = EngineState::Closing;
        cancel_connect.store(true, std::memory_order_release);
        if (candidate_stream) {
            candidate_stream->close();
        }
        active = current;
    }
    if (active) {
        set_terminal(active, DisconnectReason::LocalClose);
        request_close(active, DisconnectReason::LocalClose);
    }
    if (dispatcher_thread.joinable() && dispatcher_thread.get_id() == std::this_thread::get_id()) {
        return {};
    }
    {
        std::unique_lock lock(lifecycle_mutex);
        lifecycle_cv.wait(lock, [this] { return lifecycle == EngineState::Idle; });
    }
    join_workers();
    return {};
}

inline void EngineImpl::maintenance_loop(const std::shared_ptr<EngineSession>& session) {
    while (!session->close_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (session->close_requested.load(std::memory_order_acquire)) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto now_ticks = ticks(now);
        expire_fragments(session, now);
        if (session->config.heartbeat.disabled) {
            continue;
        }
        const auto interval = session->config.heartbeat.interval.count();
        const auto elapsed = now_ticks - session->last_activity.load(std::memory_order_acquire);
        if (elapsed >= interval * static_cast<std::int64_t>(session->config.heartbeat.missed_interval_limit)) {
            set_terminal(session, DisconnectReason::HeartbeatTimeout, Protocol::error(
                ErrorKind::Timeout,
                "session heartbeat deadline expired",
                StatusCode::ERR_READ_TIMEOUT,
                ErrorPhase::Heartbeat,
                session->view.epoch
            ));
            request_close(session, DisconnectReason::HeartbeatTimeout);
            break;
        }
        const auto since_heartbeat = now_ticks - session->last_heartbeat.load(std::memory_order_acquire);
        if (since_heartbeat >= interval) {
            const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            if (auto sent = write_control(session, Json{{"type", "heartbeat"}, {"ts", utc}}); !sent) {
                set_terminal(session, DisconnectReason::TransportFailure, sent.error());
                request_close(session, DisconnectReason::TransportFailure);
                break;
            }
            session->last_heartbeat.store(now_ticks, std::memory_order_release);
        }
    }
}

inline void EngineImpl::expire_fragments(
    const std::shared_ptr<EngineSession>& session,
    std::chrono::steady_clock::time_point now
) {
    std::size_t expired{};
    {
        std::lock_guard lock(session->fragment_mutex);
        for (auto iterator = session->fragments.begin(); iterator != session->fragments.end();) {
            if (iterator->second.deadline <= now) {
                iterator = session->fragments.erase(iterator);
                ++expired;
            } else {
                ++iterator;
            }
        }
    }
    while (expired-- > 0) {
        enqueue_application(session, DispatchEvent{
            DispatchEvent::Type::Error,
            Protocol::error(
                ErrorKind::Timeout,
                "incomplete fragment sequence expired",
                StatusCode::ERR_FRAGMENT_TIMEOUT,
                ErrorPhase::Fragmentation,
                session->view.epoch
            ),
            true
        });
    }
}

}

/*
 * connect performs exactly one bounded dial and establishment attempt. close
 * cancels either the candidate or current stream, and reconnection can occur
 * only after every epoch-owned worker has returned the engine to idle.
 */
