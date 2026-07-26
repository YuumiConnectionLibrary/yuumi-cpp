#include <yuumi/bridge.hpp>
#include <yuumi/diagnostic.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <optional>
#include <string_view>
#include <thread>

namespace {
std::atomic_bool shutdown_requested{false};

void handle_sigint(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

std::optional<std::uint32_t> parse_pid(std::string_view text) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        yuumi::Diagnostic::error(
            yuumi::StatusCode::ERR_INTERNAL,
            "Usage: cpp_server <endpoint_name> <token> [expected_pid]"
        );
        return 1;
    }
    yuumi::EngineConfig config;
    config.endpoint_name = argv[1];
    config.token = argv[2];
    config.max_sessions = 4;
    if (argc == 4) {
        config.expected_pid = parse_pid(argv[3]);
        if (!config.expected_pid) {
            yuumi::Diagnostic::error(yuumi::StatusCode::ERR_INTERNAL, "expected_pid is not a uint32 value");
            return 1;
        }
    }

    yuumi::Engine engine(std::move(config));
    engine.on_message([&engine](const yuumi::MessageEvent& event) {
        if (event.channel != yuumi::Channel::Command || !event.payload.is_object() ||
            event.payload.value("action", std::string()) != "ping") {
            return;
        }
        const yuumi::Json response{{"action", "pong"}};
        const auto sent = event.correlation_id
            ? engine.send_correlated(event.session, yuumi::Channel::Data, *event.correlation_id, response)
            : engine.send(event.session, yuumi::Channel::Data, response);
        if (!sent) {
            yuumi::Diagnostic::error(sent.error().status, sent.error().cause);
        }
    });
    engine.on_error([](const yuumi::ErrorInfo& error) {
        yuumi::Diagnostic::error(error.status, error.cause);
    });

    std::signal(SIGINT, handle_sigint);
    if (auto opened = engine.open(); !opened) {
        yuumi::Diagnostic::error(opened.error().status, opened.error().cause);
        return 1;
    }
    while (!shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (auto closed = engine.close(); !closed) {
        yuumi::Diagnostic::error(closed.error().status, closed.error().cause);
        return 1;
    }
    return 0;
}

/*
 * This example is application policy, not Engine API policy. It accepts up to
 * four Go sessions and preserves an incoming correlation ID in its pong.
 */
