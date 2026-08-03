#pragma once

#include <yuumi/detail/engine_frames.hpp>

#include <exception>

namespace yuumi::detail {

inline bool EngineImpl::enqueue_application(
    const std::shared_ptr<EngineSession>& session,
    DispatchEvent event
) {
    bool overflow{};
    {
        std::lock_guard lock(session->event_mutex);
        if (!session->accepting_events.load(std::memory_order_acquire) ||
            session->finalized.load(std::memory_order_acquire)) {
            return false;
        }
        if (event.capacity && session->capacity_used >= session->config.application_queue_capacity) {
            overflow = true;
        } else {
            session->events.push_back(std::move(event));
            if (session->events.back().capacity) {
                ++session->capacity_used;
            }
        }
    }
    if (overflow) {
        session->accepting_events.store(false, std::memory_order_release);
        set_terminal(session, DisconnectReason::Backpressure, Protocol::error(
            ErrorKind::Backpressure,
            "application queue capacity exhausted",
            StatusCode::ERR_INTERNAL,
            ErrorPhase::ApplicationDispatch,
            session->view.epoch
        ));
        request_close(session, DisconnectReason::Backpressure);
        return false;
    }
    session->event_cv.notify_one();
    return true;
}

inline void EngineImpl::enqueue_terminal(
    const std::shared_ptr<EngineSession>& session,
    DispatchEvent event
) {
    {
        std::lock_guard lock(session->event_mutex);
        session->events.push_back(std::move(event));
    }
    session->event_cv.notify_one();
}

inline void EngineImpl::dispatcher_loop(const std::shared_ptr<EngineSession>& session) {
    for (;;) {
        DispatchEvent event;
        {
            std::unique_lock lock(session->event_mutex);
            session->event_cv.wait(lock, [&] {
                return !session->events.empty() ||
                       session->transport_finalized.load(std::memory_order_acquire);
            });
            if (session->events.empty()) {
                session->dispatch_drained = true;
                session->event_cv.notify_all();
                return;
            }
            event = std::move(session->events.front());
            session->events.pop_front();
        }
        try {
            dispatch_event(session, event);
        } catch (...) {
            if (event.type == DispatchEvent::Type::Error ||
                event.type == DispatchEvent::Type::Disconnected) {
                std::terminate();
            }
            std::string cause = "application callback raised a non-standard exception";
            try {
                throw;
            } catch (const std::exception& exception) {
                cause = exception.what();
            } catch (...) {
            }
            {
                std::lock_guard lock(session->event_mutex);
                session->events.push_front(DispatchEvent{
                    DispatchEvent::Type::Error,
                    Protocol::error(
                        ErrorKind::Application,
                        std::move(cause),
                        StatusCode::ERR_INTERNAL,
                        ErrorPhase::ApplicationDispatch,
                        session->view.epoch
                    ),
                    false
                });
            }
        }
        if (event.capacity) {
            std::lock_guard lock(session->event_mutex);
            --session->capacity_used;
        }
    }
}

inline void EngineImpl::dispatch_event(
    const std::shared_ptr<EngineSession>&,
    const DispatchEvent& event
) {
    SessionConnectedHandler connected;
    MessageHandler message;
    HeartbeatHandler heartbeat;
    ErrorHandler error;
    SessionDisconnectedHandler disconnected;
    {
        std::lock_guard lock(handler_mutex);
        connected = connected_handler;
        message = message_handler;
        heartbeat = heartbeat_handler;
        error = error_handler;
        disconnected = disconnected_handler;
    }
    switch (event.type) {
        case DispatchEvent::Type::Connected:
            if (connected) {
                connected(std::get<SessionView>(event.value));
            }
            break;
        case DispatchEvent::Type::Message:
            if (message) {
                message(std::get<MessageEvent>(event.value));
            }
            break;
        case DispatchEvent::Type::Heartbeat:
            if (heartbeat) {
                heartbeat(std::get<HeartbeatEvent>(event.value));
            }
            break;
        case DispatchEvent::Type::Error:
            if (error) {
                error(std::get<ErrorInfo>(event.value));
            }
            break;
        case DispatchEvent::Type::Disconnected:
            if (disconnected) {
                disconnected(std::get<DisconnectEvent>(event.value));
            }
            break;
    }
}

inline void EngineImpl::emit_pre_session_error(const ErrorInfo& failure) {
    ErrorHandler handler;
    {
        std::lock_guard lock(handler_mutex);
        handler = error_handler;
    }
    if (!handler) {
        return;
    }
    try {
        handler(failure);
    } catch (...) {
        std::terminate();
    }
}

inline void EngineImpl::finalize_session(const std::shared_ptr<EngineSession>& session) {
    bool expected = false;
    if (!session->finalized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    session->accepting_events.store(false, std::memory_order_release);
    session->close_requested.store(true, std::memory_order_release);
    {
        std::lock_guard lock(session->fragment_mutex);
        session->fragments.clear();
    }
    {
        std::lock_guard lock(session->responder_mutex);
        for (const auto& weak : session->responders) {
            if (const auto responder = weak.lock()) {
                responder->invalidate();
            }
        }
        session->responders.clear();
    }
    {
        std::lock_guard lock(lifecycle_mutex);
        if (current == session) {
            current.reset();
        }
        candidate_stream.reset();
        lifecycle = EngineState::Idle;
    }
    lifecycle_cv.notify_all();

    TerminalResult terminal;
    {
        std::lock_guard lock(session->terminal_mutex);
        terminal = session->terminal;
    }
    if (terminal.error) {
        enqueue_terminal(session, DispatchEvent{
            DispatchEvent::Type::Error,
            *terminal.error,
            false
        });
    }
    enqueue_terminal(session, DispatchEvent{
        DispatchEvent::Type::Disconnected,
        DisconnectEvent{session->view, terminal},
        false
    });
    session->transport_finalized.store(true, std::memory_order_release);
    session->event_cv.notify_all();
}

}

/*
 * The bounded queue separates IPC progress from application handlers. Accepted
 * events drain serially; backpressure reserves terminal error and disconnect
 * delivery, callback failures stay ahead of later events, and responder/session
 * state is cleared before idle becomes visible.
 */
