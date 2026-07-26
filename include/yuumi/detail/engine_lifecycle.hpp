#pragma once

#include <yuumi/detail/engine_state.hpp>

namespace yuumi::detail {

inline Result<> EngineImpl::validate_config() const {
    if (!valid_endpoint_name(config.endpoint_name)) {
        return unexpected(configuration_error("invalid endpoint_name"));
    }
    if (!valid_token(config.token)) {
        return unexpected(configuration_error("invalid token"));
    }
    if (config.max_sessions <= 0) {
        return unexpected(configuration_error("max_sessions must be greater than zero"));
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
        return unexpected(configuration_error("supported_capabilities enables an unimplemented bit"));
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

inline Result<> EngineImpl::open() {
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    if (lifecycle != Lifecycle::Closed) {
        return unexpected(Protocol::error(
            ErrorCategory::Endpoint,
            StatusCode::ERR_PIPE_FAILED,
            ErrorPhase::EndpointOpen,
            "engine is already open or changing state"
        ));
    }
    lifecycle = Lifecycle::Opening;
    lifecycle_lock.unlock();
    if (auto valid = validate_config(); !valid) {
        lifecycle_lock.lock();
        lifecycle = Lifecycle::Closed;
        lifecycle_cv.notify_all();
        return valid;
    }
    auto address = resolve_transport_address(config.endpoint_name, config.token);
    if (!address) {
        lifecycle_lock.lock();
        lifecycle = Lifecycle::Closed;
        lifecycle_cv.notify_all();
        return unexpected(address.error());
    }
    auto new_listener = std::make_shared<Listener>();
    if (auto opened = new_listener->open(*address); !opened) {
        lifecycle_lock.lock();
        lifecycle = Lifecycle::Closed;
        lifecycle_cv.notify_all();
        return opened;
    }
    listener = std::move(new_listener);
    accepting.store(true, std::memory_order_release);
    auto self = shared_from_this();
    accept_thread = std::thread([self] { self->accept_loop(); });
    maintenance_thread = std::thread([self] { self->maintenance_loop(); });
    lifecycle_lock.lock();
    lifecycle = Lifecycle::Open;
    lifecycle_cv.notify_all();
    return {};
}

inline Result<> EngineImpl::close() {
    if (callback_owner == this) {
        return unexpected(Protocol::error(
            ErrorCategory::Internal,
            StatusCode::ERR_INTERNAL,
            ErrorPhase::Close,
            "close cannot run synchronously from an Engine callback"
        ));
    }
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    if (lifecycle == Lifecycle::Closed) {
        return {};
    }
    if (lifecycle == Lifecycle::Closing) {
        lifecycle_cv.wait(lifecycle_lock, [this] { return lifecycle == Lifecycle::Closed; });
        return {};
    }
    if (lifecycle == Lifecycle::Opening) {
        lifecycle_cv.wait(lifecycle_lock, [this] { return lifecycle != Lifecycle::Opening; });
        if (lifecycle == Lifecycle::Closed) {
            return {};
        }
    }
    lifecycle = Lifecycle::Closing;
    accepting.store(false, std::memory_order_release);
    lifecycle_lock.unlock();
    if (listener) {
        listener->close();
    }
    std::vector<std::shared_ptr<EngineSession>> active;
    {
        std::lock_guard sessions_lock(sessions_mutex);
        active.assign(connections.begin(), connections.end());
    }
    for (const auto& session : active) {
        request_close(session, DisconnectReason::EngineClose);
    }
    maintenance_cv.notify_all();
    if (accept_thread.joinable()) {
        accept_thread.join();
    }
    if (maintenance_thread.joinable()) {
        maintenance_thread.join();
    }
    {
        std::unique_lock worker_lock(worker_mutex);
        worker_cv.wait(worker_lock, [this] { return active_workers == 0; });
    }
    {
        std::lock_guard sessions_lock(sessions_mutex);
        sessions.clear();
        connections.clear();
    }
    listener.reset();
    lifecycle_lock.lock();
    lifecycle = Lifecycle::Closed;
    lifecycle_cv.notify_all();
    return {};
}

inline void EngineImpl::accept_loop() {
    while (accepting.load(std::memory_order_acquire)) {
        auto accepted = listener->accept();
        if (!accepted) {
            if (accepting.load(std::memory_order_acquire)) {
                emit_error(Protocol::error(
                    ErrorCategory::Endpoint,
                    StatusCode::ERR_PIPE_FAILED,
                    ErrorPhase::Accept,
                    accepted.error()
                ));
            }
            continue;
        }
        auto session = std::make_shared<EngineSession>(*accepted);
        if (test_hooks && test_hooks->fail_accept.exchange(false, std::memory_order_acq_rel)) {
            session->stream->close();
            emit_error(Protocol::error(
                ErrorCategory::Endpoint,
                StatusCode::ERR_PIPE_FAILED,
                ErrorPhase::Accept,
                "injected accept failure"
            ));
            continue;
        }
        bool admitted{};
        {
            std::lock_guard sessions_lock(sessions_mutex);
            admitted = connections.size() < static_cast<std::size_t>(config.max_sessions);
            if (admitted) {
                connections.insert(session);
            }
        }
        if (!admitted) {
            session->stream->close();
            continue;
        }
        {
            std::lock_guard worker_lock(worker_mutex);
            ++active_workers;
        }
        auto self = shared_from_this();
        std::thread([self, session] {
            self->session_loop(session);
            {
                std::lock_guard worker_lock(self->worker_mutex);
                --self->active_workers;
            }
            self->worker_cv.notify_all();
        }).detach();
    }
}

inline void EngineImpl::maintenance_loop() {
    std::unique_lock maintenance_lock(maintenance_mutex);
    while (accepting.load(std::memory_order_acquire)) {
        maintenance_cv.wait_for(maintenance_lock, std::chrono::milliseconds(50));
        if (!accepting.load(std::memory_order_acquire)) {
            break;
        }
        maintenance_lock.unlock();
        maintain_sessions();
        maintenance_lock.lock();
    }
}

inline std::vector<std::shared_ptr<EngineSession>> EngineImpl::established_sessions() {
    std::vector<std::shared_ptr<EngineSession>> result;
    std::lock_guard sessions_lock(sessions_mutex);
    result.reserve(sessions.size());
    for (const auto& [identifier, session] : sessions) {
        static_cast<void>(identifier);
        result.push_back(session);
    }
    return result;
}

inline void EngineImpl::maintain_sessions() {
    const auto now = std::chrono::steady_clock::now();
    const auto now_ticks = ticks(now);
    for (const auto& session : established_sessions()) {
        if (session->close_requested.load(std::memory_order_acquire)) {
            continue;
        }
        expire_fragments(session, now);
        if (config.heartbeat.disabled) {
            continue;
        }
        const auto interval = config.heartbeat.interval.count();
        const auto elapsed = now_ticks - session->last_activity.load(std::memory_order_acquire);
        if (elapsed >= interval * static_cast<std::int64_t>(config.heartbeat.missed_interval_limit)) {
            report_terminal(session, Protocol::error(
                ErrorCategory::Transport,
                StatusCode::ERR_READ_TIMEOUT,
                ErrorPhase::Heartbeat,
                "session heartbeat deadline expired",
                session->handle
            ));
            request_close(session, DisconnectReason::HeartbeatTimeout);
            continue;
        }
        const auto since_heartbeat = now_ticks - session->last_heartbeat.load(std::memory_order_acquire);
        if (since_heartbeat >= interval) {
            const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            if (auto sent = write_control(session, Json{{"type", "heartbeat"}, {"ts", utc}}); !sent) {
                report_terminal(session, sent.error());
                request_close(session, DisconnectReason::TransportFailure);
            } else {
                session->last_heartbeat.store(now_ticks, std::memory_order_release);
            }
        }
    }
}

inline void EngineImpl::expire_fragments(
    const std::shared_ptr<EngineSession>& session,
    std::chrono::steady_clock::time_point now
) {
    std::size_t expired{};
    {
        std::lock_guard fragment_lock(session->fragment_mutex);
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
        emit_session_error(session, Protocol::error(
            ErrorCategory::Protocol,
            StatusCode::ERR_FRAGMENT_TIMEOUT,
            ErrorPhase::Fragmentation,
            "incomplete fragment sequence expired",
            session->handle
        ));
    }
}

}

/*
 * Open validates before endpoint mutation, then starts independent accept and
 * maintenance workers. Close stops admission first and does not complete until
 * every admitted worker has released its slot and disconnect notification.
 */
