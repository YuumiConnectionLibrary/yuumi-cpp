#include <yuumi/bridge.hpp>
#include <yuumi/diagnostic.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace {
    std::mutex g_shutdown_mutex;
    std::condition_variable g_shutdown_cv;
    std::atomic_bool g_shutdown_requested{false};
    std::optional<std::reference_wrapper<yuumi::ServerBridge>> g_bridge;

    std::expected<uint32_t, std::string_view> parse_expected_pid(int argc, char* argv[]) {
        if (argc < 2) {
            return std::unexpected("Usage: cpp_server <expected_pid>");
        }
        return static_cast<uint32_t>(std::atoi(argv[1]));
    }

    void request_shutdown() {
        const bool was_requested = g_shutdown_requested.exchange(true, std::memory_order_acq_rel);
        if (!was_requested && g_bridge.has_value()) {
            g_bridge->get().stop();
        }
        g_shutdown_cv.notify_all();
    }

    void handle_sigint(int) {
        request_shutdown();
    }
} // namespace

int main(int argc, char* argv[]) {
    auto expected_pid = parse_expected_pid(argc, argv);
    if (!expected_pid) {
        yuumi::Diagnostic::error(yuumi::StatusCode::ERR_INTERNAL, std::string(expected_pid.error()));
        return 1;
    }

    yuumi::ServerBridge bridge;
    g_bridge = std::ref(bridge);

    bridge.on_message([&bridge](const yuumi::Json& payload, yuumi::Channel channel) {
        if (channel == yuumi::Channel::Command &&
            payload.is_object() &&
            payload.value("action", std::string()) == "ping") {
            bridge.send({{"action", "pong"}}, yuumi::Channel::Data);
            return;
        }

        if (channel == yuumi::Channel::Control) {
            yuumi::Diagnostic::log(yuumi::StatusCode::OK_MESSAGE_RECEIVED, payload.dump());
        }
    });

    bridge.on_error([&bridge](yuumi::Error error) {
        yuumi::Diagnostic::error(
            static_cast<yuumi::StatusCode>(error),
            std::string(yuumi::to_string(error))
        );
        bridge.stop();
        request_shutdown();
    });

    std::signal(SIGINT, handle_sigint);

    if (auto started = bridge.start("my-service", static_cast<uint32_t>(std::atoi(argv[1]))); !started) {
        yuumi::Diagnostic::error(
            static_cast<yuumi::StatusCode>(started.error()),
            std::string(yuumi::to_string(started.error()))
        );
        g_bridge.reset();
        return 1;
    }

    {
        std::unique_lock lock(g_shutdown_mutex);
        g_shutdown_cv.wait(lock, [] {
            return g_shutdown_requested.load(std::memory_order_acquire);
        });
    }

    bridge.stop();
    g_bridge.reset();
    return 0;
}

/* creato: 2026-05-28 | cpp_server | Fase 2 */
