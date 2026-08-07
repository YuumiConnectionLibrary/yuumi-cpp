#include <yuumi/bridge.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;

std::string required_environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || length <= 1) {
        std::free(value);
        throw std::runtime_error(std::string("missing ") + name);
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("missing ") + name);
    }
    return std::string(value);
#endif
}

std::string kind_name(yuumi::ErrorKind kind) {
    switch (kind) {
        case yuumi::ErrorKind::StaleEpoch: return "stale_epoch";
        case yuumi::ErrorKind::Protocol: return "protocol";
        case yuumi::ErrorKind::SessionClosed: return "session_closed";
        case yuumi::ErrorKind::Backpressure: return "backpressure";
        default: return "unexpected";
    }
}
}

int main() {
    try {
        const std::string scenario = required_environment("YUUMI_INTEROP_SCENARIO");
        const std::string engine_name = required_environment("YUUMI_INTEROP_ENGINE_NAME");
        yuumi::EngineConfig config;
        config.endpoint_name = required_environment("YUUMI_ENDPOINT_NAME");
        config.token = required_environment("YUUMI_TOKEN");
        config.connect_timeout = 5s;
        config.application_queue_capacity = scenario == "backpressure" ? 1 : 64;
        config.heartbeat.disabled = scenario != "slow";
        config.heartbeat.interval = 50ms;
        config.heartbeat.missed_interval_limit = 20;
        config.fragmentation.timeout = 150ms;

        yuumi::Engine engine(std::move(config));
        std::mutex mutex;
        std::condition_variable changed;
        std::shared_ptr<yuumi::Responder> captured;
        std::optional<yuumi::ErrorKind> terminal_kind;
        std::optional<std::string> failure;
        int disconnect_count = 0;
        bool close_requested = false;

        const auto event = [&engine_name](std::string name) {
            return yuumi::Json{{"event", std::move(name)}, {"engine", engine_name}};
        };
        const auto record = [&](std::string message) {
            std::lock_guard lock(mutex);
            if (!failure) {
                failure = std::move(message);
            }
            changed.notify_all();
        };
        const auto checked_send = [&](yuumi::Channel channel, const yuumi::Json& payload) {
            if (auto sent = engine.send(channel, payload); !sent) {
                record(sent.error().cause);
            }
        };

        engine.on_message([&](const yuumi::MessageEvent& message) {
            if (message.payload.is_string()) {
                auto payload = event("limit");
                payload["size"] = message.payload.get_ref<const std::string&>().size();
                checked_send(yuumi::Channel::Data, payload);
                return;
            }
            const auto operation = message.payload.value("op", std::string());
            if (operation == "roundtrip") {
                auto response = event("response");
                response["opaque"] = message.payload.at("opaque");
                if (!message.responder) {
                    record("roundtrip did not carry a responder");
                } else if (auto sent = message.responder->respond(response); !sent) {
                    record(sent.error().cause);
                }
                checked_send(yuumi::Channel::Log, event("uncorrelated"));
            } else if (operation == "fragmented") {
                auto payload = event("fragmented");
                payload["size"] = message.payload.value("payload", std::string()).size();
                checked_send(yuumi::Channel::Data, payload);
            } else if (operation == "slow") {
                std::this_thread::sleep_for(300ms);
                if (message.responder) {
                    if (auto sent = message.responder->respond(event("slow_complete")); !sent) {
                        record(sent.error().cause);
                    }
                }
            } else if (operation == "hold") {
                std::this_thread::sleep_for(500ms);
            } else if (operation == "capture") {
                {
                    std::lock_guard lock(mutex);
                    captured = message.responder;
                }
                checked_send(yuumi::Channel::Data, event("captured"));
            } else if (operation == "reuse" && message.responder) {
                if (auto sent = message.responder->respond(event("reuse_response")); !sent) {
                    record(sent.error().cause);
                    return;
                }
                const auto reused = message.responder->respond({{"unexpected", true}});
                auto payload = event("responder_reuse");
                payload["kind"] = reused ? "not_rejected" : kind_name(reused.error().kind);
                checked_send(yuumi::Channel::Data, payload);
            } else if (operation == "engine_close" && message.responder) {
                if (auto sent = message.responder->respond(event("closing")); !sent) {
                    record(sent.error().cause);
                }
                {
                    std::lock_guard lock(mutex);
                    close_requested = true;
                }
                changed.notify_all();
            }
        });
        engine.on_error([&](const yuumi::ErrorInfo& error) {
            {
                std::lock_guard lock(mutex);
                terminal_kind = error.kind;
            }
            if (error.status == yuumi::StatusCode::ERR_FRAGMENT_TIMEOUT) {
                auto payload = event("fragment_timeout");
                payload["code"] = static_cast<std::uint32_t>(*error.status);
                checked_send(yuumi::Channel::Data, payload);
            }
        });
        engine.on_session_disconnected([&](const yuumi::DisconnectEvent&) {
            {
                std::lock_guard lock(mutex);
                ++disconnect_count;
            }
            changed.notify_all();
        });

        if (auto connected = engine.connect(); !connected) {
            throw std::runtime_error(connected.error().cause);
        }

        if (scenario == "reconnect") {
            {
                std::unique_lock lock(mutex);
                if (!changed.wait_for(lock, 20s, [&] { return disconnect_count >= 1 || failure; })) {
                    throw std::runtime_error("timed out waiting for first disconnect");
                }
            }
            if (auto connected = engine.connect(); !connected) {
                throw std::runtime_error(connected.error().cause);
            }
            std::shared_ptr<yuumi::Responder> old_responder;
            {
                std::lock_guard lock(mutex);
                old_responder = captured;
            }
            const auto stale = old_responder
                ? old_responder->respond({{"unexpected", true}})
                : yuumi::Result<>{yuumi::unexpected(yuumi::ErrorInfo{
                      yuumi::ErrorKind::Internal,
                      "missing captured responder",
                      std::nullopt,
                      std::nullopt,
                      std::nullopt
                  })};
            const auto invalid_send = engine.send(
                yuumi::Channel::Command,
                {{"unexpected", true}}
            );
            auto payload = event("stale");
            payload["responder"] = stale ? "not_rejected" : kind_name(stale.error().kind);
            payload["send"] = invalid_send
                ? "not_rejected"
                : kind_name(invalid_send.error().kind);
            checked_send(yuumi::Channel::Data, payload);
        }

        {
            std::unique_lock lock(mutex);
            const auto target = scenario == "reconnect" ? 2 : 1;
            while (disconnect_count < target && !failure) {
                if (close_requested) {
                    lock.unlock();
                    if (auto closed = engine.close(); !closed) {
                        record(closed.error().cause);
                    }
                    lock.lock();
                }
                if (!changed.wait_for(lock, 20s, [&] {
                        return disconnect_count >= target || close_requested || failure;
                    })) {
                    failure = "timed out waiting for terminal disconnect";
                }
            }
        }
        if (auto closed = engine.close(); !closed) {
            record(closed.error().cause);
        }
        if (scenario == "oversize" && terminal_kind != yuumi::ErrorKind::Protocol) {
            record("oversize did not terminate with protocol");
        }
        if (scenario == "backpressure" && terminal_kind != yuumi::ErrorKind::Backpressure) {
            record("queue saturation did not terminate with backpressure");
        }
        if (failure) {
            throw std::runtime_error(*failure);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

/*
 * This private target applies only integration-test scenario policy. It uses
 * the public C++ Engine API and keeps reconnect explicit in the fixture.
 */
