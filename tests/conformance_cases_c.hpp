#pragma once

#include "conformance_cases_b.hpp"

namespace yuumi::testkit {

inline std::vector<std::byte> raw_frame(
    Channel channel,
    std::uint8_t flags,
    std::span<const std::byte> payload
) {
    auto framed = Protocol::frame(channel, flags, payload);
    require(framed.has_value(), "test harness could not construct a bounded frame");
    return *framed;
}

inline std::vector<std::byte> json_frame(Channel channel, const Json& payload) {
    auto encoded = Protocol::encode_payload(payload, Encoding::JSON);
    require(encoded.has_value(), "test harness could not encode JSON");
    return raw_frame(channel, Protocol::None, *encoded);
}

inline void expect_fatal(
    OpenEngine& opened,
    Peer& peer,
    std::span<const std::byte> frame,
    StatusCode expected
) {
    require(peer.write(frame).state == detail::IoState::Complete, "fatal frame write failed");
    const auto control_frame = peer.read_frame();
    require(control_frame.channel == Channel::Control, "fatal protocol error did not return Control");
    auto control = Protocol::decode_control(control_frame.payload);
    require(control && control->value("type", std::string()) == "error", "fatal output is not a Control error");
    require(control->value("code", 0U) == static_cast<std::uint32_t>(expected), "fatal Control error code differs");
    opened.events.wait([&] { return !opened.events.errors.empty(); }, "fatal error event missing");
    std::lock_guard lock(opened.events.mutex);
    require(opened.events.errors.back().status == expected, "fatal error event status differs");
}

inline void run_frame_case(int id) {
    auto value = config(id == 44 ? 2 : 1);
    value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
    if (id == 43) {
        value.fragmentation.timeout = std::chrono::milliseconds(50);
    }
    if (id == 44) {
        value.fragmentation.active_sequence_limit = 1;
    }
    auto hooks = std::make_shared<detail::EngineTestHooks>();
    OpenEngine opened(std::move(value), hooks);
    auto peer = establish(opened, id == 45 ? "handshake_cap_correlation" : "handshake_valid");
    if (id == 35) {
        send_vector(peer, "frame_channel_command");
        wait_messages(opened, 1);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages[0].channel == Channel::Command && !opened.events.messages[0].correlation_id, "JSON command dispatch differs");
        return;
    }
    if (id == 36) {
        peer.close();
        auto msgpack_value = config();
        msgpack_value.supported_encodings = {Encoding::MsgPack};
        OpenEngine msgpack(std::move(msgpack_value));
        auto msgpack_peer = establish(msgpack);
        auto encoded = Protocol::encode_payload(Json{{"codec", "msgpack"}}, Encoding::MsgPack);
        auto frame = raw_frame(Channel::Command, Protocol::None, *encoded);
        static_cast<void>(msgpack_peer.write(frame));
        wait_messages(msgpack, 1);
        std::lock_guard lock(msgpack.events.mutex);
        require(msgpack.events.messages[0].payload.value("codec", std::string()) == "msgpack", "MessagePack dispatch differs");
        return;
    }
    if (id == 37 || id == 49) {
        expect_fatal(opened, peer, fixture("frame_oversized"), StatusCode::ERR_PAYLOAD_TOO_LARGE);
        require(hooks->payload_read_attempts.load(std::memory_order_acquire) == 0, "oversized frame attempted a payload read");
        require(hooks->largest_payload_allocation.load(std::memory_order_acquire) == 0, "oversized frame allocated its declared payload");
        return;
    }
    if (id == 38) {
        auto invalid = fixture("frame_channel_command");
        invalid[5] = std::byte{0x08};
        expect_fatal(opened, peer, invalid, StatusCode::ERR_PROTOCOL_VIOLATION);
        return;
    }
    if (id == 39) {
        auto invalid = fixture("frame_channel_command");
        invalid[4] = std::byte{0x02};
        expect_fatal(opened, peer, invalid, StatusCode::ERR_PROTOCOL_VIOLATION);
        return;
    }
    if (id == 40) {
        const std::array<std::byte, 1> malformed{std::byte{'{'}};
        const auto invalid = raw_frame(Channel::Command, Protocol::None, malformed);
        expect_fatal(opened, peer, invalid, StatusCode::ERR_PROTOCOL_VIOLATION);
        return;
    }
    if (id == 41) {
        send_vector(peer, "frame_fragment_first");
        {
            std::lock_guard lock(opened.events.mutex);
            require(opened.events.messages.empty(), "first fragment dispatched prematurely");
        }
        send_vector(peer, "frame_fragment_last");
        wait_messages(opened, 1);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages[0].payload == "Hello World", "uncorrelated reassembly differs");
        return;
    }
    if (id == 42) {
        std::vector<std::byte> first_payload;
        Protocol::append_u32(first_payload, 9);
        first_payload.resize(MAX_MESSAGE_SIZE, std::byte{'a'});
        auto first_frame = raw_frame(Channel::Data, Protocol::Fragment, first_payload);
        require(peer.write(first_frame).state == detail::IoState::Complete, "boundary fragment write failed");
        std::vector<std::byte> last_payload;
        Protocol::append_u32(last_payload, 9);
        last_payload.insert(last_payload.end(), 5, std::byte{'b'});
        auto last_frame = raw_frame(Channel::Data, Protocol::Fragment | Protocol::LastFragment, last_payload);
        expect_fatal(opened, peer, last_frame, StatusCode::ERR_PAYLOAD_TOO_LARGE);
        return;
    }
    if (id == 43) {
        send_vector(peer, "frame_fragment_first");
        opened.events.wait(
            [&] { return std::ranges::any_of(opened.events.errors, [](const auto& error) { return error.status == StatusCode::ERR_FRAGMENT_TIMEOUT; }); },
            "fragment timeout event missing"
        );
        send_vector(peer, "frame_fragment_first");
        send_vector(peer, "frame_fragment_last");
        wait_messages(opened, 1);
        return;
    }
    if (id == 44) {
        auto other = establish(opened);
        send_vector(peer, "frame_fragment_first");
        send_vector(other, "frame_fragment_first");
        auto conflict = fixture("frame_fragment_first");
        set_u32(conflict, 6, 2);
        expect_fatal(opened, peer, conflict, StatusCode::ERR_PROTOCOL_VIOLATION);
        send_vector(other, "frame_fragment_last");
        wait_messages(opened, 1);
        return;
    }
    auto first = fixture("frame_fragment_correlated_first");
    auto last = fixture("frame_fragment_correlated_last");
    set_u32(last, 10, 43);
    static_cast<void>(peer.write(first));
    expect_fatal(opened, peer, last, StatusCode::ERR_PROTOCOL_VIOLATION);
}

inline void run_control_case(int id) {
    auto value = config(id == 47 ? 2 : 1);
    if (id != 46) {
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
    }
    if (id == 47) {
        value.heartbeat.disabled = false;
        value.heartbeat.interval = std::chrono::milliseconds(40);
        value.heartbeat.missed_interval_limit = 3;
    }
    OpenEngine opened(std::move(value));
    auto first = establish(opened);
    if (id == 46) {
        send_vector(first, "control_heartbeat");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages.empty() && opened.events.errors.empty(), "valid JSON heartbeat reached application or failed");
        return;
    }
    if (id == 47) {
        auto active = establish(opened);
        for (int iteration = 0; iteration < 4; ++iteration) {
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            send_vector(active, "control_heartbeat");
        }
        opened.events.wait(
            [&] { return std::ranges::any_of(opened.events.disconnected, [](const auto& event) { return event.reason == DisconnectReason::HeartbeatTimeout; }); },
            "silent session heartbeat timeout missing"
        );
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.disconnected.size() == 1, "heartbeat timeout closed an active session");
        return;
    }
    if (id == 48) {
        send_vector(first, "control_ping");
        const auto pong = first.read_frame();
        auto decoded = Protocol::decode_control(pong.payload);
        require(decoded && decoded->value("type", std::string()) == "pong" && decoded->value("seq", 0U) == 1U, "pong did not preserve ping sequence");
        return;
    }
    if (id == 49) {
        expect_fatal(opened, first, fixture("frame_oversized"), StatusCode::ERR_PAYLOAD_TOO_LARGE);
        return;
    }
    if (id == 50) {
        const auto unknown = json_frame(Channel::Control, Json{{"type", "future-control"}});
        static_cast<void>(first.write(unknown));
        send_vector(first, "frame_channel_command");
        wait_messages(opened, 1);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.errors.empty(), "unknown valid Control type produced an error");
        return;
    }
    const std::array<std::byte, 1> malformed{std::byte{'{'}};
    expect_fatal(opened, first, raw_frame(Channel::Control, Protocol::None, malformed), StatusCode::ERR_PROTOCOL_VIOLATION);
}

}
