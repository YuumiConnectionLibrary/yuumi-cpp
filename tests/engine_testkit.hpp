#pragma once

#include <yuumi/bridge.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace yuumi::detail {

struct EngineTestAccess {
    static void install(Engine& engine, std::shared_ptr<EngineTestHooks> hooks) {
        engine.impl_->test_hooks = std::move(hooks);
    }
};

}

namespace yuumi::testkit {

inline constexpr auto TEST_TIMEOUT = std::chrono::seconds(2);
inline constexpr auto POLL_INTERVAL = std::chrono::milliseconds(5);

inline void install_test_hooks(Engine& engine, std::shared_ptr<detail::EngineTestHooks> hooks) {
    detail::EngineTestAccess::install(engine, std::move(hooks));
}

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

inline EngineConfig config() {
    static std::atomic<std::uint32_t> sequence{1};
    const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
    EngineConfig result;
    result.endpoint_name = "ct-" + hex8(process_id()) + "-" + hex8(value);
    result.token = std::string(24, '0') + hex8(value);
    result.heartbeat.disabled = true;
    result.connect_timeout = TEST_TIMEOUT;
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

inline std::vector<std::byte> handshake(std::string_view name = "handshake_valid") {
    auto bytes = fixture(name);
    if (bytes.size() >= 12) {
        set_u32(bytes, 8, process_id());
    }
    return bytes;
}

struct Frame {
    Channel channel{Channel::Control};
    std::uint8_t flags{};
    std::vector<std::byte> payload;
};

class Peer {
public:
    Peer() = default;
    explicit Peer(std::shared_ptr<detail::Stream> value) : stream_(std::move(value)) {}

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
        require(length <= MAX_MESSAGE_SIZE, "test listener received an oversized frame");
        Frame result;
        result.channel = static_cast<Channel>(std::to_integer<std::uint8_t>(header[4]));
        result.flags = std::to_integer<std::uint8_t>(header[5]);
        result.payload.resize(length);
        require(read(result.payload).state == detail::IoState::Complete, "expected a complete frame payload");
        return result;
    }

    void write_frame(Channel channel, std::uint8_t flags, std::span<const std::byte> payload) {
        auto packet = Protocol::frame(channel, flags, payload);
        require(packet.has_value(), "could not build test frame");
        require(write(*packet).state == detail::IoState::Complete, "could not write test frame");
    }

    void write_control(const Json& value) {
        auto packet = Protocol::control_frame(value);
        require(packet.has_value(), "could not build test Control frame");
        require(write(*packet).state == detail::IoState::Complete, "could not write test Control frame");
    }

    void close() {
        if (stream_) {
            stream_->close();
        }
    }

private:
    std::shared_ptr<detail::Stream> stream_;
};

class GoListener {
public:
    explicit GoListener(const EngineConfig& value) {
        auto derived = detail::resolve_transport_address(value.endpoint_name, value.token);
        require(derived.has_value(), "test listener address derivation failed");
        address_ = *derived;
        open();
    }

    GoListener(const GoListener&) = delete;
    GoListener& operator=(const GoListener&) = delete;

    ~GoListener() {
        close();
    }

    Peer accept() {
#ifdef _WIN32
        const auto handle = pending_.load(std::memory_order_acquire);
        require(handle != INVALID_HANDLE_VALUE, "test Named Pipe is not open");
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(operation.hEvent != nullptr, "could not create test accept event");
        const auto connected = ConnectNamedPipe(handle, &operation) != FALSE;
        auto code = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && code == ERROR_IO_PENDING) {
            WaitForSingleObject(operation.hEvent, INFINITE);
            DWORD transferred{};
            code = GetOverlappedResult(handle, &operation, &transferred, FALSE)
                ? ERROR_SUCCESS : GetLastError();
        } else if (!connected && code == ERROR_PIPE_CONNECTED) {
            code = ERROR_SUCCESS;
        }
        CloseHandle(operation.hEvent);
        require(code == ERROR_SUCCESS, "test Named Pipe accept failed: " + std::to_string(code));
        pending_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        return Peer(std::make_shared<detail::Stream>(handle));
#else
        const auto connection = ::accept(descriptor_.load(std::memory_order_acquire), nullptr, nullptr);
        require(connection >= 0, "test Unix listener accept failed");
        return Peer(std::make_shared<detail::Stream>(connection));
#endif
    }

    const std::string& address() const noexcept {
        return address_;
    }

    void close() {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
#ifdef _WIN32
        const auto handle = pending_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
        if (handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle, nullptr);
            CloseHandle(handle);
        }
#else
        const auto descriptor = descriptor_.exchange(-1, std::memory_order_acq_rel);
        if (descriptor >= 0) {
            shutdown(descriptor, SHUT_RDWR);
            ::close(descriptor);
        }
        if (!address_.empty()) {
            unlink(address_.c_str());
        }
#endif
    }

private:
    void open() {
#ifdef _WIN32
        const std::wstring address(address_.begin(), address_.end());
        const auto handle = CreateNamedPipeW(
            address.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            64U * 1024U,
            64U * 1024U,
            0,
            nullptr
        );
        require(handle != INVALID_HANDLE_VALUE, "could not create test Named Pipe");
        pending_.store(handle, std::memory_order_release);
#else
        sockaddr_un endpoint{};
        endpoint.sun_family = AF_UNIX;
        std::memcpy(endpoint.sun_path, address_.c_str(), address_.size() + 1);
        unlink(address_.c_str());
        const auto descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
        require(descriptor >= 0, "could not create test Unix listener");
        require(bind(descriptor, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0,
                "could not bind test Unix listener");
        require(chmod(address_.c_str(), S_IRUSR | S_IWUSR) == 0,
                "could not protect test Unix listener");
        require(listen(descriptor, 1) == 0, "could not listen on test Unix socket");
        descriptor_.store(descriptor, std::memory_order_release);
#endif
        closed_.store(false, std::memory_order_release);
    }

    std::string address_;
    std::atomic_bool closed_{true};
#ifdef _WIN32
    std::atomic<HANDLE> pending_{INVALID_HANDLE_VALUE};
#else
    std::atomic<int> descriptor_{-1};
#endif
};

struct ConnectedPeer {
    Peer peer;
    std::array<std::byte, 4> acknowledgement{};
    Frame assignment;
};

inline std::pair<SessionView, ConnectedPeer> establish(
    Engine& engine,
    GoListener& listener,
    std::vector<std::byte> handshake_bytes = handshake()
) {
    std::promise<ConnectedPeer> promised;
    auto ready = promised.get_future();
    std::thread peer_thread([&listener, bytes = std::move(handshake_bytes), promised = std::move(promised)]() mutable {
        try {
            auto peer = listener.accept();
            require(peer.write(bytes).state == detail::IoState::Complete, "could not send handshake");
            ConnectedPeer result;
            result.peer = std::move(peer);
            require(result.peer.read(result.acknowledgement).state == detail::IoState::Complete,
                    "expected handshake acknowledgement");
            result.assignment = result.peer.read_frame();
            promised.set_value(std::move(result));
        } catch (...) {
            promised.set_exception(std::current_exception());
        }
    });
    const auto connected = engine.connect();
    if (!connected) {
        peer_thread.join();
        throw Failure("engine connect failed: " + connected.error().cause);
    }
    auto peer = ready.get();
    peer_thread.join();
    require(peer.assignment.channel == Channel::Control && peer.assignment.flags == 0,
            "session assignment ordering is invalid");
    auto assignment = Protocol::decode_control(peer.assignment.payload);
    require(assignment && assignment->value("type", std::string()) == "session",
            "missing session assignment");
    require(assignment->value("session_id", std::string()) == connected->session_id,
            "session assignment does not match API view");
    return {*connected, std::move(peer)};
}

struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<SessionView> connected;
    std::vector<MessageEvent> messages;
    std::vector<HeartbeatEvent> heartbeats;
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
        engine.on_heartbeat([this](const HeartbeatEvent& event) {
            std::lock_guard lock(mutex);
            heartbeats.push_back(event);
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
        require(changed.wait_for(lock, TEST_TIMEOUT, predicate), std::move(message));
    }
};

template <typename Predicate>
inline void wait_until(Predicate predicate, std::string message) {
    const auto deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Failure(std::move(message));
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

}

/*
 * This private testkit deliberately implements the Go-role listener so the
 * public C++ package remains dialer-only. It owns endpoint creation and cleanup
 * solely for conformance tests and consumes canonical vectors at runtime.
 */
