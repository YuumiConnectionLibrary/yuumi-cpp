#include "engine_testkit.hpp"

#include <charconv>
#include <iostream>
#include <iterator>
#include <set>

namespace yuumi::testkit {

inline void write_vector(Peer& peer, std::string_view name) {
    const auto bytes = fixture(name);
    require(peer.write(bytes).state == detail::IoState::Complete, "could not write canonical vector");
}

inline Result<SessionView> rejected_handshake(
    Engine& engine,
    GoListener& listener,
    std::vector<std::byte> bytes,
    bool close_after_write = false
) {
    std::promise<detail::IoState> promised;
    auto observed = promised.get_future();
    std::thread peer_thread([&listener, bytes = std::move(bytes), close_after_write, promised = std::move(promised)]() mutable {
        try {
            auto peer = listener.accept();
            require(peer.write(bytes).state == detail::IoState::Complete, "could not write rejected handshake");
            if (close_after_write) {
                peer.close();
                promised.set_value(detail::IoState::Closed);
                return;
            }
            std::array<std::byte, 1> output{};
            promised.set_value(peer.read(output).state);
        } catch (...) {
            promised.set_exception(std::current_exception());
        }
    });
    auto result = engine.connect();
    const auto state = observed.get();
    peer_thread.join();
    require(!result, "invalid handshake unexpectedly connected");
    require(state == detail::IoState::Closed, "invalid handshake produced wire output");
    require(engine.state() == EngineState::Idle, "failed handshake did not restore idle");
    return result;
}

inline void case_configuration() {
    auto value = config();
    value.endpoint_name.clear();
    Engine engine(value);
    auto hooks = std::make_shared<detail::EngineTestHooks>();
    install_test_hooks(engine, hooks);
    const auto result = engine.connect();
    require(!result && result.error().kind == ErrorKind::Configuration, "invalid config was not typed");
    require(hooks->dial_attempts.load() == 0, "invalid config dialed");
    EngineConfig defaults;
    require(defaults.connect_timeout == std::chrono::seconds(10), "connect timeout default changed");
    require(defaults.application_queue_capacity == 64, "queue capacity default changed");
}

inline void case_address() {
    const auto token = std::string("000102030405060708090a0b0c0d0e0f");
    auto address = detail::resolve_transport_address("a", token);
    require(address.has_value(), "minimum address vector failed");
#ifdef _WIN32
    require(*address == R"(\\.\pipe\yuumi-a-000102030405060708090a0b0c0d0e0f)", "Windows address vector mismatch");
#else
    require(std::filesystem::path(*address).filename() == "yuumi-c3da2f7decb02b7a24e054711453a85c.sock", "Unix digest vector mismatch");
#endif
    require(!detail::resolve_transport_address("_bad", token), "invalid name derived an address");
}

inline void case_native_dial() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    auto [view, connected] = establish(engine, listener);
    require(view.epoch == 1 && !view.session_id.empty(), "native dial did not establish");
    connected.peer.close();
    engine.close();
}

inline void case_dial_failure() {
    auto value = config();
    Engine engine(value);
    const auto failed = engine.connect();
    require(!failed && failed.error().kind == ErrorKind::Dial, "absent endpoint was not a dial error");
    GoListener listener(value);
    auto [view, peer] = establish(engine, listener);
    require(view.epoch == 1, "explicit retry did not establish first epoch");
    peer.peer.close();
    engine.close();
}

inline void case_handshake_invalid() {
    for (const auto name : {"handshake_bad_magic", "handshake_bad_version"}) {
        auto value = config();
        GoListener listener(value);
        Engine engine(value);
        rejected_handshake(engine, listener, handshake(name));
    }
    {
        auto value = config();
        value.supported_encodings = {Encoding::JSON};
        GoListener listener(value);
        Engine engine(value);
        const auto rejected = rejected_handshake(
            engine,
            listener,
            handshake("handshake_encoding_unsupported")
        );
        require(rejected.error().kind == ErrorKind::Encoding, "unsupported encoding error kind mismatch");
    }
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    auto short_packet = handshake();
    short_packet.pop_back();
    rejected_handshake(engine, listener, std::move(short_packet), true);
}

inline void case_negotiation() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
    GoListener listener(value);
    Engine engine(value);
    auto [view, connected] = establish(engine, listener, handshake("handshake_cap_correlation"));
    require(view.encoding == Encoding::JSON, "encoding preference was not selected");
    require(view.capabilities == CAP_CORRELATION, "capability intersection mismatch");
    require(connected.acknowledgement == std::array<std::byte, 4>{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{1}}, "ACK bytes mismatch");
    connected.peer.close();
    engine.close();
}

inline void case_assignment_order() {
    auto value = config();
    GoListener listener(value);
    Events events;
    Engine engine(value);
    events.attach(engine);
    auto [view, connected] = establish(engine, listener);
    events.wait([&] { return !events.connected.empty(); }, "connected event missing");
    require(events.connected.front() == view, "connected view differs from connect result");
    connected.peer.close();
    engine.close();
}

inline void case_establishment_failure() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    auto hooks = std::make_shared<detail::EngineTestHooks>();
    hooks->fail_ack_write.store(true);
    install_test_hooks(engine, hooks);
    const auto result = rejected_handshake(engine, listener, handshake());
    require(result.error().phase == ErrorPhase::AckWrite, "ACK failure phase mismatch");
    require(!engine.session(), "failed establishment exposed a session");
}

inline void case_duplicate_connect() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    auto [view, connected] = establish(engine, listener);
    const auto duplicate = engine.connect();
    require(!duplicate && duplicate.error().kind == ErrorKind::State, "connected duplicate connect was accepted");
    require(engine.session() == view, "duplicate connect changed current session");
    connected.peer.close();
    engine.close();
}

inline void case_close_reconnect() {
    auto value = config();
    Engine engine(value);
    {
        GoListener listener(value);
        auto [first, connected] = establish(engine, listener);
        require(engine.close().has_value() && engine.close().has_value(), "close is not idempotent");
        require(engine.state() == EngineState::Idle, "close did not restore idle");
    }
    GoListener replacement(value);
    auto [second, connected] = establish(engine, replacement);
    require(second.epoch > 1, "explicit reconnect did not advance epoch");
    connected.peer.close();
    engine.close();
}

inline std::pair<SessionView, ConnectedPeer> json_session(Engine& engine, GoListener& listener) {
    return establish(engine, listener, handshake("handshake_valid"));
}

inline void case_frames_oversize() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = json_session(engine, listener);
    write_vector(connected.peer, "frame_channel_command");
    events.wait([&] { return !events.messages.empty(); }, "valid frame was not dispatched");
    write_vector(connected.peer, "frame_oversized");
    const auto terminal = connected.peer.read_frame();
    require(terminal.channel == Channel::Control, "oversize did not send final Control error");
    events.wait([&] { return !events.disconnected.empty(); }, "oversize did not disconnect");
    require(events.disconnected.back().terminal.error->status == StatusCode::ERR_PAYLOAD_TOO_LARGE, "oversize status mismatch");
}

inline void case_malformed() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = establish(engine, listener);
    connected.peer.write_frame(Channel::Command, Protocol::LastFragment, std::span<const std::byte>{});
    static_cast<void>(connected.peer.read_frame());
    events.wait([&] { return !events.disconnected.empty(); }, "malformed flags did not disconnect");
    require(events.disconnected.back().terminal.error->status == StatusCode::ERR_PROTOCOL_VIOLATION, "malformed status mismatch");
}

inline void case_fragmentation() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = json_session(engine, listener);
    write_vector(connected.peer, "frame_fragment_first");
    write_vector(connected.peer, "frame_fragment_last");
    events.wait([&] { return !events.messages.empty(); }, "fragments did not reassemble");
    require(events.messages.front().payload == "Hello World", "fragment payload mismatch");
    connected.peer.close();
    engine.close();
}

inline void case_responder() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = establish(engine, listener, handshake("handshake_cap_correlation"));
    write_vector(connected.peer, "frame_correlated_request");
    events.wait([&] { return !events.messages.empty(); }, "correlated input missing");
    const auto responder = events.messages.front().responder;
    require(responder && responder->respond(Json{{"ok", true}}), "responder failed");
    const auto response = connected.peer.read_frame();
    require(response.channel == Channel::Data && (response.flags & Protocol::Correlated) != 0, "response direction/flag mismatch");
    require(!responder->respond(Json{}), "responder was reusable");
    connected.peer.close();
    engine.close();
}

inline void case_public_send() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    auto [view, connected] = json_session(engine, listener);
    require(!engine.send(Channel::Command, Json{}), "direction-invalid public send succeeded");
    require(engine.send(Channel::Log, 1) && engine.send(Channel::Data, 2), "valid public send failed");
    const auto first = connected.peer.read_frame();
    const auto second = connected.peer.read_frame();
    require(first.channel == Channel::Log && second.channel == Channel::Data, "sequential send order changed");
    connected.peer.close();
    engine.close();
}

inline void case_control() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = establish(engine, listener);
    connected.peer.write_control(Json{{"type", "ping"}, {"seq", 17}});
    const auto pong = connected.peer.read_frame();
    auto decoded = Protocol::decode_control(pong.payload);
    require(decoded && decoded->value("type", std::string()) == "pong" && decoded->value("seq", 0) == 17, "pong mismatch");
    connected.peer.write_control(Json{{"type", "future"}});
    connected.peer.close();
    engine.close();
    require(events.messages.empty(), "Control reached application message handler");
}

inline void case_slow_handler() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    engine.on_message([&](const MessageEvent&) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    auto [view, connected] = json_session(engine, listener);
    write_vector(connected.peer, "frame_channel_command");
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }), "slow handler did not start");
    }
    connected.peer.write_control(Json{{"type", "ping"}, {"seq", 9}});
    require(connected.peer.read_frame().channel == Channel::Control, "IPC stalled behind handler");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    connected.peer.close();
    engine.close();
}

inline void case_backpressure() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    value.application_queue_capacity = 2;
    GoListener listener(value);
    Engine engine(value);
    std::mutex mutex;
    std::condition_variable changed;
    bool release{};
    std::size_t messages{};
    std::vector<ErrorInfo> errors;
    std::vector<DisconnectEvent> disconnected;
    engine.on_message([&](const MessageEvent&) {
        std::unique_lock lock(mutex);
        ++messages;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    engine.on_error([&](const ErrorInfo& error) { std::lock_guard lock(mutex); errors.push_back(error); changed.notify_all(); });
    engine.on_session_disconnected([&](const DisconnectEvent& event) { std::lock_guard lock(mutex); disconnected.push_back(event); changed.notify_all(); });
    auto [view, connected] = json_session(engine, listener);
    write_vector(connected.peer, "frame_channel_command");
    write_vector(connected.peer, "frame_channel_command");
    write_vector(connected.peer, "frame_channel_command");
    wait_until([&] { return engine.state() == EngineState::Idle; }, "backpressure did not close transport");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return !disconnected.empty(); }), "backpressure terminal events missing");
        require(disconnected.back().terminal.reason == DisconnectReason::Backpressure, "backpressure terminal reason mismatch");
        require(!errors.empty() && errors.back().kind == ErrorKind::Backpressure, "backpressure error missing");
    }
    engine.close();
}

inline void case_callback_failure() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    engine.on_message([](const MessageEvent&) { throw std::runtime_error("handler failed"); });
    engine.on_error([&](const ErrorInfo& error) { std::lock_guard lock(events.mutex); events.errors.push_back(error); events.changed.notify_all(); });
    auto [view, connected] = json_session(engine, listener);
    write_vector(connected.peer, "frame_channel_command");
    events.wait([&] { return !events.errors.empty(); }, "callback failure was not observable");
    require(events.errors.back().kind == ErrorKind::Application, "callback failure kind mismatch");
    connected.peer.close();
    engine.close();
}

inline void case_disconnect() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    Events events;
    events.attach(engine);
    auto [view, connected] = establish(engine, listener);
    connected.peer.close();
    events.wait([&] { return !events.disconnected.empty(); }, "peer close was not observed");
    require(engine.state() == EngineState::Idle && !engine.session(), "disconnect did not clear session before idle");
    engine.close();
}

inline void case_stale_epoch() {
    auto value = config();
    value.supported_encodings = {Encoding::JSON};
    Engine engine(value);
    Events events;
    events.attach(engine);
    std::shared_ptr<Responder> old;
    {
        GoListener listener(value);
        auto [first, connected] = establish(engine, listener, handshake("handshake_cap_correlation"));
        write_vector(connected.peer, "frame_correlated_request");
        events.wait([&] { return !events.messages.empty(); }, "old responder missing");
        old = events.messages.back().responder;
        connected.peer.close();
        wait_until([&] { return engine.state() == EngineState::Idle; }, "old epoch did not close");
    }
    GoListener replacement(value);
    auto [second, connected] = establish(engine, replacement, handshake("handshake_cap_correlation"));
    require(second.epoch > old->epoch(), "replacement epoch did not increase");
    const auto stale = old->respond(Json{});
    require(!stale && stale.error().kind == ErrorKind::StaleEpoch, "stale responder was not rejected");
    connected.peer.close();
    engine.close();
}

inline void case_error_kinds() {
    const std::array kinds{
        ErrorKind::Configuration, ErrorKind::AddressDerivation, ErrorKind::Dial, ErrorKind::Timeout,
        ErrorKind::Handshake, ErrorKind::Protocol, ErrorKind::Encoding, ErrorKind::Capability,
        ErrorKind::Backpressure, ErrorKind::SessionClosed, ErrorKind::StaleEpoch, ErrorKind::Application,
        ErrorKind::Transport, ErrorKind::Internal, ErrorKind::State
    };
    std::set<int> unique;
    for (const auto kind : kinds) unique.insert(static_cast<int>(kind));
    require(unique.size() == kinds.size(), "Engine error kinds are not distinguishable");
}

inline std::string public_headers() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "include" / "yuumi";
    std::string content;
    for (const auto name : {"bridge.hpp", "transport.hpp", "types.hpp"}) {
        std::ifstream input(root / name);
        content.append(std::istreambuf_iterator<char>(input), {});
    }
    return content;
}

inline void case_public_surface() {
    const auto headers = public_headers();
    for (const auto forbidden : {
        "class Listener", "ServerBridge", "using Bridge", "max_sessions", "accept_loop", "install_test_hooks"
    }) {
        require(headers.find(forbidden) == std::string::npos, std::string("legacy public symbol remains: ") + forbidden);
    }
}

inline void case_environment_adapter() {
    const auto headers = public_headers();
    require(headers.find("from_env") == std::string::npos, "environment adapter unexpectedly performs hidden work");
    Engine engine(config());
    require(engine.state() == EngineState::Idle, "construction created transport work");
    engine.close();
}

inline void case_cleanup() {
    auto value = config();
    GoListener listener(value);
    Engine engine(value);
    auto [view, connected] = establish(engine, listener);
    require(engine.close().has_value(), "close failed");
    require(engine.state() == EngineState::Idle && !engine.session(), "close leaked session state");
    require(engine.close().has_value(), "repeated cleanup failed");

    auto callback_config = config();
    callback_config.supported_encodings = {Encoding::JSON};
    GoListener callback_listener(callback_config);
    std::shared_ptr<Engine> callback_engine = std::make_shared<Engine>(callback_config);
    std::promise<void> callback_destroyed;
    auto callback_completion = callback_destroyed.get_future();
    callback_engine->on_message([&](const MessageEvent&) {
        callback_engine.reset();
        callback_destroyed.set_value();
    });
    auto callback_peer = establish(*callback_engine, callback_listener, handshake("handshake_valid"));
    write_vector(callback_peer.second.peer, "frame_channel_command");
    require(
        callback_completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "Engine destruction from callback did not complete"
    );
    callback_completion.get();
    callback_peer.second.peer.close();
}

inline void run_case(int identifier) {
    switch (identifier) {
        case 1: case_configuration(); break;
        case 2: case_address(); break;
        case 3: case_native_dial(); break;
        case 4: case_dial_failure(); break;
        case 5: case_handshake_invalid(); break;
        case 6: case_negotiation(); break;
        case 7: case_assignment_order(); break;
        case 8: case_establishment_failure(); break;
        case 9: case_duplicate_connect(); break;
        case 10: case_close_reconnect(); break;
        case 11: case_frames_oversize(); break;
        case 12: case_malformed(); break;
        case 13: case_fragmentation(); break;
        case 14: case_responder(); break;
        case 15: case_public_send(); break;
        case 16: case_control(); break;
        case 17: case_slow_handler(); break;
        case 18: case_backpressure(); break;
        case 19: case_callback_failure(); break;
        case 20: case_disconnect(); break;
        case 21: case_stale_epoch(); break;
        case 22: case_error_kinds(); break;
        case 23: case_public_surface(); break;
        case 24: case_environment_adapter(); break;
        case 25: case_cleanup(); break;
        default: throw Failure("unknown Engine Conformance case");
    }
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected one Engine Conformance case number\n";
        return 2;
    }
    int identifier{};
    const std::string_view text(argv[1]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), identifier);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || identifier < 1 || identifier > 25) {
        std::cerr << "Engine Conformance case must be in the range 1..25\n";
        return 2;
    }
    try {
        yuumi::testkit::run_case(identifier);
    } catch (const std::exception& failure) {
        std::cerr << "EC-" << identifier << " failed: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}

/*
 * CTest invokes one executable per canonical EC-001 through EC-025 case. The
 * private Go-role listener makes the topology executable without publishing a
 * non-Go client or duplicating endpoint ownership in the engine package.
 */
