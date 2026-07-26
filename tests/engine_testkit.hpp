#pragma once

#include <yuumi/bridge.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace yuumi::testkit {

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void require(bool condition, std::string message) {
    if (!condition) {
        throw Failure(std::move(message));
    }
}

inline std::uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

inline std::string hex8(std::uint32_t value) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    };
    std::string result(8, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[result.size() - index - 1] = digits[value & 0xFU];
        value >>= 4U;
    }
    return result;
}

inline EngineConfig config(std::int64_t max_sessions = 1) {
    static std::atomic<std::uint32_t> sequence{1};
    const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
    EngineConfig result;
    result.endpoint_name = "ct-" + hex8(process_id()) + "-" + hex8(value);
    result.token = std::string(24, '0') + hex8(value);
    result.max_sessions = max_sessions;
    result.heartbeat.disabled = true;
    return result;
}

inline std::vector<std::byte> fixture(std::string_view name) {
    const auto path = std::filesystem::path(YUUMI_SPEC_VECTOR_DIR) / (std::string(name) + ".bin");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.is_open(), "cannot open canonical vector " + path.string());
    const auto length = input.tellg();
    require(length >= 0, "cannot determine canonical vector size");
    input.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    require(input.good() || input.eof(), "cannot read canonical vector " + path.string());
    return bytes;
}

inline void set_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    std::vector<std::byte> encoded;
    Protocol::append_u32(encoded, value);
    std::copy(encoded.begin(), encoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

struct Frame {
    Channel channel{Channel::Control};
    std::uint8_t flags{};
    std::vector<std::byte> payload;
};

class Peer {
public:
    explicit Peer(std::shared_ptr<detail::Stream> value) : stream_(std::move(value)) {}

    static Peer connect(const std::string& address) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        std::string last_error;
        do {
            auto connected = detail::connect(address);
            if (connected) {
                return Peer(*connected);
            }
            last_error = connected.error();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        throw Failure("cannot connect private test peer: " + last_error);
    }

    detail::IoResult write(std::span<const std::byte> bytes) {
        return stream_->write_exact(bytes);
    }

    detail::IoResult read(std::span<std::byte> bytes) {
        return stream_->read_exact(bytes);
    }

    Frame read_frame() {
        std::array<std::byte, 6> header{};
        require(read(header).state == detail::IoState::Complete, "expected a complete frame header");
        const auto length = Protocol::read_u32(std::span<const std::byte, 4>(header.data(), 4));
        require(length <= MAX_MESSAGE_SIZE, "test peer received an oversized frame");
        Frame result;
        result.channel = static_cast<Channel>(std::to_integer<std::uint8_t>(header[4]));
        result.flags = std::to_integer<std::uint8_t>(header[5]);
        result.payload.resize(length);
        require(read(result.payload).state == detail::IoState::Complete, "expected a complete frame payload");
        return result;
    }

    std::array<std::byte, 4> handshake(
        std::vector<std::byte> bytes,
        bool preserve_vector_pid = true
    ) {
        if (!preserve_vector_pid) {
            set_u32(bytes, 8, process_id());
        }
        require(write(bytes).state == detail::IoState::Complete, "cannot write handshake");
        std::array<std::byte, 4> acknowledgement{};
        require(read(acknowledgement).state == detail::IoState::Complete, "expected handshake ACK");
        const auto assignment = read_frame();
        require(assignment.channel == Channel::Control && assignment.flags == 0, "session assignment ordering is invalid");
        auto control = Protocol::decode_control(assignment.payload);
        require(control && control->value("type", std::string()) == "session", "missing session assignment");
        require(!control->value("session_id", std::string()).empty(), "session identifier is empty");
        return acknowledgement;
    }

    std::shared_ptr<detail::Stream> stream() const {
        return stream_;
    }

    void close() {
        stream_->close();
    }

private:
    std::shared_ptr<detail::Stream> stream_;
};

struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<SessionView> connected;
    std::vector<MessageEvent> messages;
    std::vector<ErrorInfo> errors;
    std::vector<DisconnectEvent> disconnected;

    void attach(Engine& engine) {
        engine.on_session_connected([this](const SessionView& event) {
            std::lock_guard lock(mutex);
            connected.push_back(event);
            changed.notify_all();
        });
        engine.on_message([this](const MessageEvent& event) {
            std::lock_guard lock(mutex);
            messages.push_back(event);
            changed.notify_all();
        });
        engine.on_error([this](const ErrorInfo& event) {
            std::lock_guard lock(mutex);
            errors.push_back(event);
            changed.notify_all();
        });
        engine.on_session_disconnected([this](const DisconnectEvent& event) {
            std::lock_guard lock(mutex);
            disconnected.push_back(event);
            changed.notify_all();
        });
    }

    template <typename Predicate>
    void wait(Predicate predicate, std::string message) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(2), predicate), std::move(message));
    }
};

struct OpenEngine {
    explicit OpenEngine(
        EngineConfig value,
        std::shared_ptr<detail::EngineTestHooks> hooks = {}
    ) : config(std::move(value)), engine(config) {
        if (hooks) {
            engine.install_test_hooks(std::move(hooks));
        }
        events.attach(engine);
        auto result = engine.open();
        require(result.has_value(), "engine open failed: " + (result ? std::string() : result.error().cause));
        auto derived = resolve_transport_address(config.endpoint_name, config.token);
        require(derived.has_value(), "address derivation failed after valid open");
        address = *derived;
    }

    ~OpenEngine() {
        static_cast<void>(engine.close());
    }

    EngineConfig config;
    Engine engine;
    Events events;
    std::string address;
};

inline SessionView wait_connected(OpenEngine& opened, std::size_t count = 1) {
    opened.events.wait(
        [&] { return opened.events.connected.size() >= count; },
        "connected event deadline expired"
    );
    return opened.events.connected[count - 1];
}

inline Peer establish(
    OpenEngine& opened,
    std::string_view handshake_name = "handshake_valid",
    bool preserve_vector_pid = true
) {
    std::size_t expected_count{};
    {
        std::lock_guard lock(opened.events.mutex);
        expected_count = opened.events.connected.size() + 1;
    }
    auto peer = Peer::connect(opened.address);
    static_cast<void>(peer.handshake(fixture(handshake_name), preserve_vector_pid));
    static_cast<void>(wait_connected(opened, expected_count));
    return peer;
}

}

/*
 * The private peer is test infrastructure only. It consumes canonical vectors
 * from yuumi-spec at runtime and never installs or exposes a client API.
 */
