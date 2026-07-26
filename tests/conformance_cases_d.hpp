#pragma once

#include "conformance_cases_c.hpp"

namespace yuumi::testkit {

inline std::uint32_t correlation_id(const Frame& frame) {
    require((frame.flags & Protocol::Correlated) != 0 && frame.payload.size() >= 4, "output is not correlated");
    return Protocol::read_u32(std::span<const std::byte, 4>(frame.payload.data(), 4));
}

inline void run_correlation_case(int id) {
    auto value = config(id == 56 ? 2 : 1);
    value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
    if (id == 52) {
        value.supported_capabilities = 0;
    }
    OpenEngine opened(std::move(value));
    auto peer = establish(opened, id == 52 ? "handshake_valid" : "handshake_cap_correlation");
    if (id == 52) {
        expect_fatal(opened, peer, fixture("frame_correlated_not_negotiated"), StatusCode::ERR_PROTOCOL_VIOLATION);
        return;
    }
    if (id == 53 || id == 54) {
        opened.engine.on_message([&](const MessageEvent& event) {
            const Json payload = id == 53
                ? Json{{"ok", true}}
                : Json{{"error", "application rejected request"}};
            const auto sent = opened.engine.send_correlated(
                event.session,
                Channel::Data,
                *event.correlation_id,
                payload
            );
            require(sent.has_value(), "application correlation response failed");
        });
        send_vector(peer, "frame_correlated_request");
        const auto response = peer.read_frame();
        require(response.channel == Channel::Data && correlation_id(response) == 42, "response did not preserve correlation ID");
        auto decoded = Protocol::decode_payload(
            std::span<const std::byte>(response.payload).subspan(4),
            Encoding::JSON
        );
        require(decoded.has_value(), "correlated response payload is not JSON");
        if (id == 53) {
            require(*decoded == Json{{"ok", true}}, "correlated success payload differs");
        } else {
            require(decoded->contains("error"), "application error was not sent as Data");
        }
        return;
    }
    if (id == 55) {
        send_vector(peer, "frame_fragment_correlated_first");
        send_vector(peer, "frame_fragment_correlated_last");
        wait_messages(opened, 1);
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.messages[0].payload == "Hello World" && opened.events.messages[0].correlation_id == 42, "fragment/correlation prefix order differs");
        return;
    }
    detail::PendingCorrelations pending;
    require(pending.begin(42), "first pending correlation was rejected");
    require(!pending.begin(42), "pending correlation ID was reused");
    require(pending.finish(42), "pending correlation completion failed");
    require(pending.begin(42), "released correlation ID was not reusable");
    pending.clear();
    require(!pending.finish(42), "late unmatched response matched cleared state");
}

inline void run_send_case(int id) {
    auto value = config(2);
    value.supported_encodings = {Encoding::MsgPack, Encoding::JSON};
    OpenEngine opened(std::move(value));
    auto json_handshake = fixture("handshake_valid");
    json_handshake[12] = std::byte{0x01};
    auto json_peer = Peer::connect(opened.address);
    static_cast<void>(json_peer.handshake(json_handshake));
    const auto json_session = wait_connected(opened, 1).handle;
    auto msgpack_handshake = fixture(id == 58 ? "handshake_cap_correlation" : "handshake_valid");
    msgpack_handshake[12] = std::byte{0x02};
    auto msgpack_peer = Peer::connect(opened.address);
    static_cast<void>(msgpack_peer.handshake(msgpack_handshake));
    const auto msgpack_session = wait_connected(opened, 2).handle;
    if (id == 57) {
        require(opened.engine.send(json_session, Channel::Data, Json{{"order", 1}}).has_value(), "first JSON send failed");
        require(opened.engine.send(json_session, Channel::Data, Json{{"order", 2}}).has_value(), "second JSON send failed");
        require(opened.engine.send(msgpack_session, Channel::Log, Json{{"codec", "msgpack"}}).has_value(), "MessagePack Log send failed");
        const auto first = json_peer.read_frame();
        const auto second = json_peer.read_frame();
        auto first_json = Protocol::decode_payload(first.payload, Encoding::JSON);
        auto second_json = Protocol::decode_payload(second.payload, Encoding::JSON);
        require(first_json && second_json && (*first_json)["order"] == 1 && (*second_json)["order"] == 2, "sequential send order changed");
        const auto log = msgpack_peer.read_frame();
        require(log.channel == Channel::Log && Protocol::decode_payload(log.payload, Encoding::MsgPack).has_value(), "send used wrong channel or encoding");
        return;
    }
    if (id == 58) {
        require(!opened.engine.send_correlated(json_session, Channel::Data, 42, Json{{"ok", true}}), "baseline session accepted correlated send");
        require(opened.engine.send_correlated(msgpack_session, Channel::Data, 42, Json{{"ok", true}}).has_value(), "negotiated session rejected correlated send");
        require(correlation_id(msgpack_peer.read_frame()) == 42, "correlated send changed supplied ID");
        return;
    }
    if (id == 59) {
        require(!opened.engine.send(json_session, Channel::Control, Json{{"type", "heartbeat"}}), "application forged Control");
        require(!opened.engine.send(json_session, Channel::Command, Json{{"bad", true}}), "engine sent client-only Command");
        require(!opened.engine.send(json_session, static_cast<Channel>(0xFF), Json{{"bad", true}}), "engine sent unknown channel");
        return;
    }
    if (id == 60) {
        const SessionHandle absent{"absent", 0};
        require(!opened.engine.send(absent, Channel::Data, Json{{"bad", true}}), "absent session send succeeded");
        json_peer.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "closed-session setup did not disconnect");
        require(!opened.engine.send(json_session, Channel::Data, Json{{"bad", true}}), "closed session send succeeded");
        auto stale = msgpack_session;
        ++stale.epoch;
        require(!opened.engine.send(stale, Channel::Data, Json{{"bad", true}}), "stale epoch send succeeded");
        require(opened.engine.send(msgpack_session, Channel::Data, Json{{"live", true}}).has_value(), "invalid handles affected live session");
        return;
    }
    if (id == 61) {
        Json oversized = std::string(MAX_MESSAGE_SIZE + 1, 'x');
        require(!opened.engine.send(json_session, Channel::Data, oversized), "oversized serialization succeeded");
        require(opened.engine.send(json_session, Channel::Data, Json{{"still", "usable"}}).has_value(), "size rejection closed live session");
        return;
    }
    if (id == 62) {
        json_peer.close();
        opened.events.wait([&] { return !opened.events.disconnected.empty(); }, "transport-failure setup did not disconnect");
        require(!opened.engine.send(json_session, Channel::Data, Json{{"late", true}}), "send to failed transport succeeded");
        require(opened.engine.send(msgpack_session, Channel::Data, Json{{"healthy", true}}).has_value(), "transport failure crossed sessions");
        return;
    }
    if (id == 63) {
        require(!opened.engine.send(SessionHandle{"missing", 9}, Channel::Data, Json{}), "invalid session did not preserve failure");
        return;
    }
    require(opened.engine.send(json_session, Channel::Data, Json{{"engine", true}}).has_value(), "required Engine send surface is unusable");
}

}
