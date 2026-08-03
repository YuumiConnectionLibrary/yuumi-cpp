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

void report(const yuumi::ErrorInfo& error) {
    yuumi::Diagnostic::error(error.status.value_or(yuumi::StatusCode::ERR_INTERNAL), error.cause);
}
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        yuumi::Diagnostic::error(
            yuumi::StatusCode::ERR_INTERNAL,
            "Usage: cpp_engine <endpoint_name> <token> [expected_go_pid]"
        );
        return 1;
    }
    yuumi::EngineConfig config;
    config.endpoint_name = argv[1];
    config.token = argv[2];
    if (argc == 4) {
        config.expected_go_pid = parse_pid(argv[3]);
        if (!config.expected_go_pid) {
            yuumi::Diagnostic::error(yuumi::StatusCode::ERR_INTERNAL, "expected_go_pid is not a uint32 value");
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
        const auto sent = event.responder
            ? event.responder->respond(response)
            : engine.send(yuumi::Channel::Data, response);
        if (!sent) {
            report(sent.error());
        }
    });
    engine.on_error(report);

    std::signal(SIGINT, handle_sigint);
    if (auto connected = engine.connect(); !connected) {
        report(connected.error());
        return 1;
    }
    while (!shutdown_requested.load(std::memory_order_acquire) &&
           engine.state() == yuumi::EngineState::Connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (auto closed = engine.close(); !closed) {
        report(closed.error());
        return 1;
    }
    return 0;
}

/*
 * The application supplies endpoint identity and lifecycle policy. Yuumi only
 * dials the Go listener, dispatches opaque commands, and preserves correlation
 * through the single-use responder attached to an inbound message.
 */
