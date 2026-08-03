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

    Result<SessionView> connect() {
        return impl_->connect();
    }

    Result<> close() {
        return impl_->close();
    }

    Result<> send(Channel channel, const Json& payload) {
        return impl_->send(channel, payload);
    }

    EngineState state() const {
        return impl_->state();
    }

    std::optional<SessionView> session() const {
        return impl_->session();
    }

    void on_session_connected(SessionConnectedHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->connected_handler = std::move(handler);
    }

    void on_message(MessageHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->message_handler = std::move(handler);
    }

    void on_heartbeat(HeartbeatHandler handler) {
        std::lock_guard lock(impl_->handler_mutex);
        impl_->heartbeat_handler = std::move(handler);
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

}

/*
 * Engine::connect performs one bounded dial and returns only after ACK and
 * session assignment are written. Public sends target the current epoch and
 * accept only Log or Data; correlated replies use the event Responder.
 */
