#pragma once

#include <yuumi/detail/engine_events.hpp>

namespace yuumi {

class Engine {
public:
    explicit Engine(EngineConfig config)
        : impl_(std::make_shared<detail::EngineImpl>(std::move(config))) {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    ~Engine() {
        static_cast<void>(impl_->close());
    }

    Result<> open() {
        return impl_->open();
    }

    Result<> close() {
        return impl_->close();
    }

    Result<> send(const SessionHandle& session, Channel channel, const Json& payload) {
        return impl_->send(session, channel, payload, std::nullopt);
    }

    Result<> send_correlated(
        const SessionHandle& session,
        Channel channel,
        std::uint32_t correlation_id,
        const Json& payload
    ) {
        return impl_->send(session, channel, payload, correlation_id);
    }

    void on_session_connected(SessionConnectedHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->connected_handler = std::move(handler);
    }

    void on_message(MessageHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->message_handler = std::move(handler);
    }

    void on_error(ErrorHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->error_handler = std::move(handler);
    }

    void on_session_disconnected(SessionDisconnectedHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->disconnected_handler = std::move(handler);
    }

#ifdef YUUMI_ENABLE_TESTKIT
    void install_test_hooks(std::shared_ptr<detail::EngineTestHooks> hooks) {
        impl_->test_hooks = std::move(hooks);
    }
#endif

private:
    std::shared_ptr<detail::EngineImpl> impl_;
};

using ServerBridge = Engine;
using Bridge = Engine;

}

/*
 * Engine::open returns once the secured endpoint can accept clients.
 * Every send requires a live generation-aware SessionHandle and accepts only
 * Log or Data. Sequential same-session sends are serialized; concurrent sends
 * follow mutex acquisition order. Callbacks for one session are serialized,
 * while callbacks for separate sessions may execute concurrently.
 * Synchronous close is rejected from callbacks because close waits for callback
 * completion; schedule it on the application lifecycle thread instead.
 */
