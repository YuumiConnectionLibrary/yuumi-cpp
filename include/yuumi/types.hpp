#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace yuumi {

inline constexpr std::uint32_t PROTOCOL_VERSION = 1;
inline constexpr std::uint32_t CAP_CORRELATION = 0x000001;
inline constexpr std::uint32_t IMPLEMENTED_CAPABILITIES = CAP_CORRELATION;
inline constexpr std::size_t MAX_MESSAGE_SIZE = 16U * 1024U * 1024U;

using Json = nlohmann::json;

enum class Encoding : std::uint8_t {
    JSON = 0x01,
    MsgPack = 0x02
};

enum class Channel : std::uint8_t {
    Control = 0x00,
    Command = 0x01,
    Log = 0x02,
    Data = 0x03
};

enum class StatusCode : std::uint32_t {
    HANDSHAKE_START = 100,
    CONNECTING = 101,
    OK_CONNECTED = 200,
    OK_MESSAGE_RECEIVED = 201,
    OK_HEARTBEAT = 202,
    ERR_MAGIC_MISMATCH = 400,
    ERR_VERSION_MISMATCH = 401,
    ERR_PID_MISMATCH = 402,
    ERR_PROTOCOL_VIOLATION = 403,
    ERR_FRAGMENT_TIMEOUT = 404,
    ERR_PAYLOAD_TOO_LARGE = 413,
    ERR_ENCODING_UNSUPPORTED = 415,
    ERR_PIPE_FAILED = 500,
    ERR_READ_TIMEOUT = 501,
    ERR_WRITE_FAILED = 502,
    ERR_CONNECTION_LOST = 503,
    ERR_INTERNAL = 599
};

enum class ErrorKind {
    Configuration,
    AddressDerivation,
    Dial,
    Timeout,
    Handshake,
    Protocol,
    Encoding,
    Capability,
    Backpressure,
    SessionClosed,
    StaleEpoch,
    Application,
    Transport,
    Internal,
    State
};

enum class ErrorPhase {
    Configuration,
    AddressDerivation,
    Dial,
    HandshakeRead,
    HandshakeValidate,
    AckWrite,
    SessionWrite,
    FrameRead,
    FrameDecode,
    FrameWrite,
    Heartbeat,
    Fragmentation,
    ApplicationDispatch,
    ApplicationSend,
    Close
};

enum class DisconnectReason {
    LocalClose,
    PeerClose,
    HeartbeatTimeout,
    ProtocolFailure,
    TransportFailure,
    Backpressure
};

enum class EngineState {
    Idle,
    Connecting,
    Connected,
    Closing
};

struct SessionView {
    std::string session_id;
    std::uint64_t epoch{};
    Encoding encoding{Encoding::MsgPack};
    std::uint32_t capabilities{};

    friend bool operator==(const SessionView&, const SessionView&) = default;
};

struct ErrorInfo {
    ErrorKind kind{ErrorKind::Internal};
    std::string cause;
    std::optional<StatusCode> status;
    std::optional<ErrorPhase> phase;
    std::optional<std::uint64_t> epoch;
};

template <typename E>
struct Unexpected {
    E error;
};

template <typename E>
Unexpected<std::decay_t<E>> unexpected(E&& error) {
    return {std::forward<E>(error)};
}

template <typename T, typename E>
class Expected {
public:
    Expected() requires std::is_default_constructible_v<T>
        : storage_(std::in_place_index<0>) {}

    Expected(const T& value) : storage_(std::in_place_index<0>, value) {}
    Expected(T&& value) : storage_(std::in_place_index<0>, std::move(value)) {}

    template <typename G>
    Expected(Unexpected<G> failure)
        : storage_(std::in_place_index<1>, std::move(failure.error)) {}

    bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    T& operator*() {
        return std::get<0>(storage_);
    }

    const T& operator*() const {
        return std::get<0>(storage_);
    }

    T* operator->() {
        return &std::get<0>(storage_);
    }

    const T* operator->() const {
        return &std::get<0>(storage_);
    }

    E& error() {
        return std::get<1>(storage_);
    }

    const E& error() const {
        return std::get<1>(storage_);
    }

private:
    std::variant<T, E> storage_;
};

template <typename E>
class Expected<void, E> {
public:
    Expected() = default;

    template <typename G>
    Expected(Unexpected<G> failure) : error_(std::move(failure.error)) {}

    bool has_value() const noexcept {
        return !error_.has_value();
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    E& error() {
        return *error_;
    }

    const E& error() const {
        return *error_;
    }

private:
    std::optional<E> error_;
};

template <typename T = void>
using Result = Expected<T, ErrorInfo>;

namespace detail {
class EngineImpl;
}

class Responder {
public:
    Result<> respond(const Json& payload) const {
        std::function<Result<>(const Json&)> send;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->used) {
                return unexpected(ErrorInfo{
                    ErrorKind::StaleEpoch,
                    "responder is single-use or stale",
                    StatusCode::ERR_PROTOCOL_VIOLATION,
                    ErrorPhase::ApplicationSend,
                    state_->epoch
                });
            }
            state_->used = true;
            send = state_->send;
        }
        return send(payload);
    }

    std::uint64_t epoch() const noexcept {
        return state_->epoch;
    }

    std::uint32_t correlation_id() const noexcept {
        return state_->correlation_id;
    }

private:
    struct State {
        std::mutex mutex;
        bool used{};
        std::uint64_t epoch{};
        std::uint32_t correlation_id{};
        std::function<Result<>(const Json&)> send;
    };

    explicit Responder(std::shared_ptr<State> state) : state_(std::move(state)) {}

    void invalidate() const {
        std::lock_guard lock(state_->mutex);
        state_->used = true;
    }

    std::shared_ptr<State> state_;

    friend class detail::EngineImpl;
};

struct MessageEvent {
    SessionView session;
    Channel channel{Channel::Data};
    Json payload;
    std::optional<std::uint32_t> correlation_id;
    std::shared_ptr<Responder> responder;
};

struct HeartbeatEvent {
    SessionView session;
    std::int64_t timestamp{};
};

struct TerminalResult {
    DisconnectReason reason{DisconnectReason::PeerClose};
    std::optional<ErrorInfo> error;
};

struct DisconnectEvent {
    SessionView session;
    TerminalResult terminal;
};

struct HeartbeatSettings {
    bool disabled{false};
    std::chrono::milliseconds interval{std::chrono::seconds(30)};
    std::uint32_t missed_interval_limit{3};
};

struct FragmentationSettings {
    std::chrono::milliseconds timeout{std::chrono::seconds(15)};
    std::size_t active_sequence_limit{16};
};

struct EngineConfig {
    std::string endpoint_name;
    std::string token;
    std::vector<Encoding> supported_encodings{Encoding::MsgPack, Encoding::JSON};
    std::uint32_t supported_capabilities{CAP_CORRELATION};
    std::optional<std::uint32_t> expected_go_pid;
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(10)};
    std::size_t application_queue_capacity{64};
    HeartbeatSettings heartbeat;
    FragmentationSettings fragmentation;
};

using SessionConnectedHandler = std::function<void(const SessionView&)>;
using MessageHandler = std::function<void(const MessageEvent&)>;
using HeartbeatHandler = std::function<void(const HeartbeatEvent&)>;
using ErrorHandler = std::function<void(const ErrorInfo&)>;
using SessionDisconnectedHandler = std::function<void(const DisconnectEvent&)>;

constexpr std::string_view to_string(StatusCode code) {
    switch (code) {
        case StatusCode::HANDSHAKE_START: return "handshake start";
        case StatusCode::CONNECTING: return "connecting";
        case StatusCode::OK_CONNECTED: return "connected";
        case StatusCode::OK_MESSAGE_RECEIVED: return "message received";
        case StatusCode::OK_HEARTBEAT: return "heartbeat";
        case StatusCode::ERR_MAGIC_MISMATCH: return "magic mismatch";
        case StatusCode::ERR_VERSION_MISMATCH: return "version mismatch";
        case StatusCode::ERR_PID_MISMATCH: return "PID mismatch";
        case StatusCode::ERR_PROTOCOL_VIOLATION: return "protocol violation";
        case StatusCode::ERR_FRAGMENT_TIMEOUT: return "fragment timeout";
        case StatusCode::ERR_PAYLOAD_TOO_LARGE: return "payload exceeds 16 MiB";
        case StatusCode::ERR_ENCODING_UNSUPPORTED: return "encoding unsupported";
        case StatusCode::ERR_PIPE_FAILED: return "local transport failed";
        case StatusCode::ERR_READ_TIMEOUT: return "read timeout";
        case StatusCode::ERR_WRITE_FAILED: return "write failed";
        case StatusCode::ERR_CONNECTION_LOST: return "connection lost";
        case StatusCode::ERR_INTERNAL: return "internal error";
    }
    return "unknown status";
}

}

/*
 * Public Engine API values are deliberately transport-neutral.
 * Configuration has protocol-conforming defaults and uses Disabled rather than
 * Enabled so its zero-value boolean keeps heartbeat emission active.
 * SessionView exposes the wire-visible identifier and local epoch while
 * responder authority remains single-use and bound to that epoch.
 */
