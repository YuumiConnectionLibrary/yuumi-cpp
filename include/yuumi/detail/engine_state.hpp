#pragma once

#include <yuumi/transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

namespace yuumi::detail {

struct FragmentState {
    Channel channel{Channel::Data};
    std::uint32_t fragment_id{};
    std::optional<std::uint32_t> correlation_id;
    std::vector<std::byte> data;
    std::chrono::steady_clock::time_point deadline;
};

struct DispatchEvent {
    enum class Type {
        Connected,
        Message,
        Heartbeat,
        Error,
        Disconnected
    };

    Type type{Type::Error};
    std::variant<SessionView, MessageEvent, HeartbeatEvent, ErrorInfo, DisconnectEvent> value;
    bool capacity{};
};

struct EngineTestHooks {
    std::atomic<bool> fail_dial{false};
    std::atomic<bool> fail_ack_write{false};
    std::atomic<bool> fail_session_write{false};
    std::atomic<bool> fail_frame_read{false};
    std::atomic<std::size_t> dial_attempts{0};
    std::atomic<std::size_t> payload_read_attempts{0};
    std::atomic<std::size_t> largest_payload_allocation{0};
};

struct EngineSession {
    EngineSession(std::shared_ptr<Stream> value, SessionView session_view, EngineConfig snapshot)
        : stream(std::move(value)), view(std::move(session_view)), config(std::move(snapshot)) {}

    std::shared_ptr<Stream> stream;
    SessionView view;
    EngineConfig config;
    std::atomic<bool> close_requested{false};
    std::atomic<bool> finalized{false};
    std::atomic<bool> accepting_events{true};
    std::atomic<bool> transport_finalized{false};
    std::atomic<std::int64_t> last_activity{};
    std::atomic<std::int64_t> last_heartbeat{};
    std::mutex fragment_mutex;
    std::unordered_map<std::uint64_t, FragmentState> fragments;
    std::mutex responder_mutex;
    std::vector<std::weak_ptr<Responder>> responders;
    std::mutex terminal_mutex;
    TerminalResult terminal;
    std::mutex event_mutex;
    std::condition_variable event_cv;
    std::deque<DispatchEvent> events;
    std::size_t capacity_used{};
    bool dispatch_drained{};
};

class EngineImpl : public std::enable_shared_from_this<EngineImpl> {
public:
    explicit EngineImpl(EngineConfig value) : source_config(std::move(value)) {}
    ~EngineImpl();

    Result<> validate_config(const EngineConfig& config) const;
    Result<SessionView> connect();
    Result<> close();
    Result<> send(Channel channel, const Json& payload);
    Result<> respond(std::uint64_t epoch, std::uint32_t correlation_id, const Json& payload);
    EngineState state() const;
    std::optional<SessionView> session() const;

    Result<SessionView> establish(
        const std::shared_ptr<Stream>& stream,
        const EngineConfig& config,
        Deadline deadline
    );
    void reader_loop(const std::shared_ptr<EngineSession>& session);
    void maintenance_loop(const std::shared_ptr<EngineSession>& session);
    void dispatcher_loop(const std::shared_ptr<EngineSession>& session);
    void finalize_session(const std::shared_ptr<EngineSession>& session);
    void join_workers();

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
    bool decode_and_enqueue(
        const std::shared_ptr<EngineSession>& session,
        Channel channel,
        std::optional<std::uint32_t> correlation_id,
        std::span<const std::byte> payload
    );
    void protocol_failure(
        const std::shared_ptr<EngineSession>& session,
        StatusCode status,
        ErrorPhase phase,
        std::string cause
    );
    Result<> write_control(const std::shared_ptr<EngineSession>& session, const Json& value);
    Result<> write_packet(
        const std::shared_ptr<EngineSession>& session,
        std::span<const std::byte> packet,
        ErrorPhase phase
    );
    Result<> send_packet(
        const std::shared_ptr<EngineSession>& session,
        Channel channel,
        const Json& payload,
        std::optional<std::uint32_t> correlation_id
    );
    void request_close(const std::shared_ptr<EngineSession>& session, DisconnectReason reason);
    void set_terminal(
        const std::shared_ptr<EngineSession>& session,
        DisconnectReason reason,
        std::optional<ErrorInfo> failure = std::nullopt
    );
    void expire_fragments(
        const std::shared_ptr<EngineSession>& session,
        std::chrono::steady_clock::time_point now
    );

    bool enqueue_application(const std::shared_ptr<EngineSession>& session, DispatchEvent event);
    void enqueue_terminal(const std::shared_ptr<EngineSession>& session, DispatchEvent event);
    void dispatch_event(const std::shared_ptr<EngineSession>& session, const DispatchEvent& event);
    void emit_pre_session_error(const ErrorInfo& failure);

    static std::int64_t ticks(std::chrono::steady_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }

    static ErrorInfo configuration_error(std::string cause) {
        return Protocol::error(
            ErrorKind::Configuration,
            std::move(cause),
            StatusCode::ERR_PROTOCOL_VIOLATION,
            ErrorPhase::Configuration
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

    EngineConfig source_config;
    mutable std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    EngineState lifecycle{EngineState::Idle};
    std::atomic_bool cancel_connect{false};
    std::shared_ptr<Stream> candidate_stream;
    std::shared_ptr<EngineSession> current;
    std::thread reader_thread;
    std::thread maintenance_thread;
    std::thread dispatcher_thread;
    std::atomic<std::uint64_t> next_epoch{0};
    std::atomic<std::uint64_t> next_session_id{1};
    std::mutex handler_mutex;
    SessionConnectedHandler connected_handler;
    MessageHandler message_handler;
    HeartbeatHandler heartbeat_handler;
    ErrorHandler error_handler;
    SessionDisconnectedHandler disconnected_handler;
    std::shared_ptr<EngineTestHooks> test_hooks;
};

}

/*
 * EngineImpl owns at most one dial attempt or established session. Transport,
 * maintenance, and serial application dispatch have separate RAII threads;
 * all queued work and responder authority are scoped to one local epoch.
 */
