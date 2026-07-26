#pragma once

#include <yuumi/transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace yuumi::detail {

struct FragmentState {
    Channel channel{Channel::Data};
    std::uint32_t fragment_id{};
    bool correlated{};
    std::optional<std::uint32_t> correlation_id;
    std::vector<std::byte> data;
    std::chrono::steady_clock::time_point deadline;
};

class PendingCorrelations {
public:
    bool begin(std::uint32_t identifier) {
        std::lock_guard lock(mutex_);
        return identifiers_.insert(identifier).second;
    }

    bool finish(std::uint32_t identifier) {
        std::lock_guard lock(mutex_);
        return identifiers_.erase(identifier) == 1;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        identifiers_.clear();
    }

private:
    std::mutex mutex_;
    std::unordered_set<std::uint32_t> identifiers_;
};

struct EngineTestHooks {
    std::atomic<bool> fail_accept{false};
    std::atomic<bool> fail_ack_write{false};
    std::atomic<bool> fail_session_write{false};
    std::atomic<bool> fail_frame_read{false};
    std::atomic<std::size_t> payload_read_attempts{0};
    std::atomic<std::size_t> largest_payload_allocation{0};
};

struct EngineSession {
    explicit EngineSession(std::shared_ptr<Stream> value) : stream(std::move(value)) {}

    std::shared_ptr<Stream> stream;
    SessionHandle handle;
    Encoding encoding{Encoding::MsgPack};
    std::uint32_t capabilities{};
    std::atomic<bool> established{false};
    std::atomic<bool> close_requested{false};
    std::atomic<bool> terminal_error_reported{false};
    std::atomic<DisconnectReason> close_reason{DisconnectReason::TransportFailure};
    std::atomic<std::int64_t> last_activity{};
    std::atomic<std::int64_t> last_heartbeat{};
    std::mutex send_mutex;
    std::mutex event_mutex;
    std::mutex fragment_mutex;
    std::unordered_map<std::uint64_t, FragmentState> fragments;
    PendingCorrelations pending_correlations;
};

class EngineImpl : public std::enable_shared_from_this<EngineImpl> {
public:
    enum class Lifecycle {
        Closed,
        Opening,
        Open,
        Closing
    };

    explicit EngineImpl(EngineConfig value) : config(std::move(value)) {}

    Result<> validate_config() const;
    Result<> open();
    Result<> close();
    Result<> send(
        const SessionHandle& handle,
        Channel channel,
        const Json& payload,
        std::optional<std::uint32_t> correlation_id
    );
    void accept_loop();
    void maintenance_loop();
    void maintain_sessions();
    void expire_fragments(
        const std::shared_ptr<EngineSession>& session,
        std::chrono::steady_clock::time_point now
    );
    Result<> establish(const std::shared_ptr<EngineSession>& session);
    void session_loop(const std::shared_ptr<EngineSession>& session);
    void release_session(const std::shared_ptr<EngineSession>& session);
    bool process_frame(
        const std::shared_ptr<EngineSession>& session,
        Channel channel,
        std::uint8_t flags,
        std::span<const std::byte> payload
    );
    bool process_control(
        const std::shared_ptr<EngineSession>& session,
        std::span<const std::byte> payload
    );
    bool process_application(
        const std::shared_ptr<EngineSession>& session,
        Channel channel,
        std::uint8_t flags,
        std::span<const std::byte> payload
    );
    bool decode_and_emit(
        const std::shared_ptr<EngineSession>& session,
        Channel channel,
        std::optional<std::uint32_t> correlation_id,
        std::span<const std::byte> payload
    );
    void protocol_failure(
        const std::shared_ptr<EngineSession>& session,
        StatusCode status,
        std::string cause
    );
    Result<> write_control(const std::shared_ptr<EngineSession>& session, const Json& value);
    Result<> write_packet(
        const std::shared_ptr<EngineSession>& session,
        std::span<const std::byte> packet,
        ErrorPhase phase
    );
    void request_close(const std::shared_ptr<EngineSession>& session, DisconnectReason reason);
    void report_terminal(const std::shared_ptr<EngineSession>& session, ErrorInfo failure);
    void emit_connected(const std::shared_ptr<EngineSession>& session);
    void emit_message(const std::shared_ptr<EngineSession>& session, const MessageEvent& event);
    void emit_error(const ErrorInfo& failure);
    void emit_session_error(const std::shared_ptr<EngineSession>& session, const ErrorInfo& failure);
    void emit_disconnected(const std::shared_ptr<EngineSession>& session);
    std::shared_ptr<EngineSession> live_session(const SessionHandle& handle);
    std::vector<std::shared_ptr<EngineSession>> established_sessions();

    static std::int64_t ticks(std::chrono::steady_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }

    static ErrorInfo configuration_error(std::string cause) {
        return Protocol::error(
            ErrorCategory::Configuration,
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration,
            std::move(cause)
        );
    }

    static std::string session_identifier(std::uint64_t value) {
        std::array<char, 16> digits{};
        digits.fill('0');
        std::array<char, 16> encoded{};
        const auto converted = std::to_chars(encoded.data(), encoded.data() + encoded.size(), value, 16);
        const auto count = static_cast<std::size_t>(converted.ptr - encoded.data());
        std::copy(encoded.data(), encoded.data() + count, digits.data() + (digits.size() - count));
        return "session-" + std::string(digits.data(), digits.size());
    }

    static inline thread_local const EngineImpl* callback_owner = nullptr;
    EngineConfig config;
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    Lifecycle lifecycle{Lifecycle::Closed};
    std::atomic<bool> accepting{false};
    std::shared_ptr<Listener> listener;
    std::thread accept_thread;
    std::thread maintenance_thread;
    std::mutex maintenance_mutex;
    std::condition_variable maintenance_cv;
    std::mutex sessions_mutex;
    std::unordered_set<std::shared_ptr<EngineSession>> connections;
    std::unordered_map<std::string, std::shared_ptr<EngineSession>> sessions;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    std::size_t active_workers{};
    std::atomic<std::uint64_t> next_epoch{0};
    std::atomic<std::uint64_t> next_session_id{1};
    std::mutex handler_mutex;
    SessionConnectedHandler connected_handler;
    MessageHandler message_handler;
    ErrorHandler error_handler;
    SessionDisconnectedHandler disconnected_handler;
    std::shared_ptr<EngineTestHooks> test_hooks;
};

}

/*
 * EngineSession owns all negotiated, heartbeat, fragmentation, correlation,
 * write-order, and event-order state for exactly one accepted connection.
 */
