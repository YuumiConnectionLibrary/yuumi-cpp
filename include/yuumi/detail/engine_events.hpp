#pragma once

#include <yuumi/detail/engine_frames.hpp>

namespace yuumi::detail {

inline void EngineImpl::emit_connected(const std::shared_ptr<EngineSession>& session) {
    SessionConnectedHandler handler;
    {
        std::lock_guard handler_lock(handler_mutex);
        handler = connected_handler;
    }
    if (handler) {
        std::lock_guard event_lock(session->event_mutex);
        callback_owner = this;
        handler(SessionView{session->handle, session->encoding, session->capabilities});
        callback_owner = nullptr;
    }
}

inline void EngineImpl::emit_message(
    const std::shared_ptr<EngineSession>& session,
    const MessageEvent& event
) {
    MessageHandler handler;
    {
        std::lock_guard handler_lock(handler_mutex);
        handler = message_handler;
    }
    if (handler) {
        std::lock_guard event_lock(session->event_mutex);
        callback_owner = this;
        handler(event);
        callback_owner = nullptr;
    }
}

inline void EngineImpl::emit_error(const ErrorInfo& failure) {
    ErrorHandler handler;
    {
        std::lock_guard handler_lock(handler_mutex);
        handler = error_handler;
    }
    if (handler) {
        callback_owner = this;
        handler(failure);
        callback_owner = nullptr;
    }
}

inline void EngineImpl::emit_session_error(
    const std::shared_ptr<EngineSession>& session,
    const ErrorInfo& failure
) {
    ErrorHandler handler;
    {
        std::lock_guard handler_lock(handler_mutex);
        handler = error_handler;
    }
    if (handler) {
        std::lock_guard event_lock(session->event_mutex);
        callback_owner = this;
        handler(failure);
        callback_owner = nullptr;
    }
}

inline void EngineImpl::emit_disconnected(const std::shared_ptr<EngineSession>& session) {
    SessionDisconnectedHandler handler;
    {
        std::lock_guard handler_lock(handler_mutex);
        handler = disconnected_handler;
    }
    if (handler) {
        std::lock_guard event_lock(session->event_mutex);
        callback_owner = this;
        handler(DisconnectEvent{session->handle, session->close_reason.load(std::memory_order_acquire)});
        callback_owner = nullptr;
    }
}

}

/*
 * Session event mutexes preserve connected/message/error/disconnected start
 * order without serializing unrelated sessions. Handlers run on the accepting
 * session worker, or on the maintenance worker for timer-generated events.
 */
