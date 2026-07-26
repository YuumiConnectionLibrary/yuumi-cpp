#pragma once

#include "conformance_cases_a.hpp"

namespace yuumi::testkit {

inline void send_vector(Peer& peer, std::string_view name) {
    require(peer.write(fixture(name)).state == detail::IoState::Complete, "cannot write canonical frame " + std::string(name));
}

inline void wait_messages(OpenEngine& opened, std::size_t count) {
    opened.events.wait([&] { return opened.events.messages.size() >= count; }, "message event deadline expired");
}

inline void run_session_case(int id) {
    if (id == 24) {
        OpenEngine opened(config());
        auto first = Peer::connect(opened.address);
        auto short_handshake = fixture("handshake_valid");
        short_handshake.resize(15);
        static_cast<void>(first.write(short_handshake));
        auto excess = Peer::connect(opened.address);
        static_cast<void>(excess.write(fixture("handshake_valid")));
        std::array<std::byte, 4> ack{};
        require(excess.read(ack).state == detail::IoState::Closed, "pre-session slot did not count toward capacity");
        first.close();
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "short handshake teardown was not observed");
        auto replacement = establish(opened);
        return;
    }
    if (id == 25) {
        OpenEngine opened(config(3));
        auto first = establish(opened);
        auto second = establish(opened);
        auto third = establish(opened);
        auto excess = Peer::connect(opened.address);
        static_cast<void>(excess.write(fixture("handshake_valid")));
        std::array<std::byte, 4> ack{};
        require(excess.read(ack).state == detail::IoState::Closed, "max_sessions accepted an excess connection");
        first.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "closed session did not release capacity");
        auto replacement = establish(opened);
        require(opened.events.connected.size() == 4, "replacement session did not establish");
        return;
    }
    if (id == 26) {
        auto value = config(2);
        value.supported_encodings = {Encoding::MsgPack, Encoding::JSON};
        OpenEngine opened(std::move(value));
        auto json_handshake = fixture("handshake_valid");
        json_handshake[12] = std::byte{0x01};
        auto first = Peer::connect(opened.address);
        static_cast<void>(first.handshake(json_handshake));
        static_cast<void>(wait_connected(opened, 1));
        auto second = Peer::connect(opened.address);
        static_cast<void>(second.handshake(fixture("handshake_cap_correlation")));
        static_cast<void>(wait_connected(opened, 2));
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.connected[0].encoding == Encoding::JSON, "first session lost JSON negotiation");
        require(opened.events.connected[1].encoding == Encoding::MsgPack, "second session lost MessagePack negotiation");
        require(opened.events.connected[0].capabilities == 0 && opened.events.connected[1].capabilities == CAP_CORRELATION, "capability state crossed sessions");
        return;
    }
    if (id == 27) {
        OpenEngine opened(config(2));
        auto first = establish(opened);
        auto second = establish(opened);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.connected[0].handle.session_id != opened.events.connected[1].handle.session_id, "active session IDs are not unique");
        require(opened.events.connected[0].handle.session_id.size() <= 128, "session ID exceeds 128 bytes");
        return;
    }
    if (id == 28) {
        auto value = config(2);
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto first = establish(opened, "handshake_cap_correlation");
        auto second = establish(opened, "handshake_cap_correlation");
        auto first_fragment = fixture("frame_fragment_correlated_first");
        auto second_fragment = first_fragment;
        const auto letter = std::ranges::find(second_fragment, std::byte{'H'});
        require(letter != second_fragment.end(), "fragment test vector has no expected data byte");
        *letter = std::byte{'J'};
        static_cast<void>(first.write(first_fragment));
        static_cast<void>(second.write(second_fragment));
        send_vector(first, "frame_fragment_correlated_last");
        send_vector(second, "frame_fragment_correlated_last");
        wait_messages(opened, 2);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages[0].session != opened.events.messages[1].session, "fragment output crossed session handles");
        require(opened.events.messages[0].payload != opened.events.messages[1].payload, "fragment buffers contaminated each other");
        return;
    }
    if (id == 29) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto first = establish(opened);
        const auto original = wait_connected(opened);
        send_vector(first, "frame_fragment_first");
        first.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "first generation did not disconnect");
        auto replacement = establish(opened);
        const auto current = wait_connected(opened, 2);
        require(current.handle.session_id != original.handle.session_id && current.handle.epoch > original.handle.epoch, "reconnection reused ID or epoch");
        send_vector(replacement, "frame_fragment_last");
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "orphan final fragment did not fail");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages.empty(), "old fragment state completed in replacement session");
        return;
    }
    if (id == 30) {
        OpenEngine opened(config());
        auto first = establish(opened);
        const auto stale = wait_connected(opened).handle;
        first.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "stale-handle setup did not disconnect");
        auto replacement = establish(opened);
        require(!opened.engine.send(stale, Channel::Data, Json{{"stale", true}}), "stale handle addressed replacement session");
        return;
    }
    if (id == 31) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto peer = establish(opened);
        send_vector(peer, "frame_channel_command");
        send_vector(peer, "frame_channel_command");
        wait_messages(opened, 2);
        peer.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "ordered disconnect event missing");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.connected.size() == 1 && opened.events.messages.size() == 2 && opened.events.disconnected.size() == 1, "per-session event cardinality differs");
        return;
    }
    if (id == 32) {
        OpenEngine opened(config());
        auto peer = establish(opened);
        peer.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "peer-close disconnect missing");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.disconnected.back().reason == DisconnectReason::PeerClose, "peer close reason differs");
        return;
    }
    if (id == 33) {
        auto value = config(3);
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        auto hooks = std::make_shared<detail::EngineTestHooks>();
        OpenEngine opened(std::move(value), hooks);
        auto failing = establish(opened);
        auto healthy = establish(opened);
        hooks->fail_accept.store(true, std::memory_order_release);
        auto rejected = Peer::connect(opened.address);
        rejected.close();
        opened.events.wait(
            [&] { return std::ranges::any_of(opened.events.errors, [](const auto& error) { return error.phase == ErrorPhase::Accept; }); },
            "injected accept failure was not isolated"
        );
        auto invalid = fixture("frame_channel_command");
        invalid[4] = std::byte{0x02};
        static_cast<void>(failing.write(invalid));
        send_vector(healthy, "frame_channel_command");
        wait_messages(opened, 1);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages.back().session == opened.events.connected[1].handle, "failure in one session redirected healthy traffic");
        return;
    }
    auto value = config();
    value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
    OpenEngine opened(std::move(value));
    auto peer = establish(opened);
    send_vector(peer, "control_heartbeat");
    send_vector(peer, "frame_fragment_first");
    send_vector(peer, "frame_fragment_last");
    wait_messages(opened, 1);
    std::lock_guard lock(opened.events.mutex);
    require(opened.events.messages.size() == 1 && opened.events.messages[0].payload == "Hello World", "only-complete-message boundary differs");
}

}
