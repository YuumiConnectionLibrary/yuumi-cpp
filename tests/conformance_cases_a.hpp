#pragma once

#include "engine_testkit.hpp"

namespace yuumi::testkit {

inline void require_ack(const std::array<std::byte, 4>& actual, std::string_view name) {
    const auto expected = fixture(name);
    require(std::ranges::equal(actual, expected), "ACK differs from canonical vector " + std::string(name));
}

inline void reject_handshake(std::string_view vector_name, StatusCode status) {
    OpenEngine opened(config());
    auto peer = Peer::connect(opened.address);
    require(peer.write(fixture(vector_name)).state == detail::IoState::Complete, "cannot write rejection vector");
    std::array<std::byte, 4> output{};
    require(peer.read(output).state == detail::IoState::Closed, "rejected handshake produced ACK bytes");
    opened.events.wait(
        [&] { return !opened.events.errors.empty(); },
        "handshake rejection error was not observable"
    );
    std::lock_guard lock(opened.events.mutex);
    require(opened.events.errors.back().status == status, "handshake rejection used the wrong status");
    require(!opened.events.errors.back().session, "pre-session error exposed a session handle");
    require(opened.events.connected.empty() && opened.events.disconnected.empty(), "rejected handshake emitted session events");
}

inline void run_configuration_case(int id) {
    if (id == 1) {
        std::vector<EngineConfig> invalid;
        auto base = config();
        auto value = base;
        value.endpoint_name.clear();
        invalid.push_back(value);
        value = base;
        value.token = "ABCDEF0123456789abcdef0123456789";
        invalid.push_back(value);
        value = base;
        value.max_sessions = 0;
        invalid.push_back(value);
        value = base;
        value.max_sessions = -1;
        invalid.push_back(value);
        value = base;
        value.supported_encodings.clear();
        invalid.push_back(value);
        value = base;
        value.supported_encodings = {Encoding::JSON, Encoding::JSON};
        invalid.push_back(value);
        value = base;
        value.supported_capabilities = 0x000002;
        invalid.push_back(value);
        value = base;
        value.heartbeat.disabled = false;
        value.heartbeat.interval = std::chrono::milliseconds(0);
        invalid.push_back(value);
        value = base;
        value.fragmentation.timeout = std::chrono::milliseconds(0);
        invalid.push_back(value);
        for (auto& candidate : invalid) {
            Engine engine(std::move(candidate));
            require(!engine.open(), "invalid configuration opened an endpoint");
        }
        return;
    }
    if (id == 2) {
        auto value = config();
        auto address = resolve_transport_address(value.endpoint_name, value.token);
        require(address.has_value(), "valid canonical address was rejected");
#ifdef _WIN32
        require(address->starts_with(R"(\\.\pipe\yuumi-)"), "Windows did not derive a Named Pipe address");
#else
        require(address->ends_with(".sock"), "Unix did not derive a socket address");
#endif
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        peer.close();
        return;
    }
    if (id == 3) {
        require(!resolve_transport_address("", std::string(32, 'a')), "empty endpoint name was accepted");
        require(!resolve_transport_address(std::string(33, 'a'), std::string(32, 'a')), "overlong endpoint name was accepted");
        require(!resolve_transport_address("valid", std::string(31, 'a')), "short token was accepted");
        require(!resolve_transport_address("valid", std::string(32, 'A')), "uppercase token was accepted");
        return;
    }
    if (id == 4) {
        auto value = config();
        value.heartbeat.disabled = false;
        require(value.max_sessions == 1, "default max_sessions is not one");
        require(value.supported_encodings == std::vector<Encoding>{Encoding::MsgPack, Encoding::JSON}, "default encoding order differs");
        require(value.supported_capabilities == CAP_CORRELATION, "default capability differs");
        require(value.heartbeat.interval == std::chrono::seconds(30) && value.heartbeat.missed_interval_limit == 3, "heartbeat defaults differ");
        OpenEngine opened(std::move(value));
        auto first = Peer::connect(opened.address);
        const auto ack = first.handshake(fixture("handshake_cap_correlation"));
        require(ack[0] == std::byte{0x02} && ack[3] == std::byte{0x01}, "default ACK did not select MessagePack with correlation");
        const auto view = wait_connected(opened);
        require(view.handle.epoch == 0 && view.encoding == Encoding::MsgPack, "default session state differs");
        auto second = Peer::connect(opened.address);
        std::array<std::byte, 4> bytes{};
        static_cast<void>(second.write(fixture("handshake_valid")));
        require(second.read(bytes).state == detail::IoState::Closed, "default capacity accepted a second session");
        return;
    }
    if (id == 5) {
        auto value = config();
        Engine engine(std::move(value));
        require(engine.open().has_value(), "first open failed");
        require(!engine.open(), "second open succeeded");
        require(engine.close().has_value(), "first close failed");
        require(engine.close().has_value(), "repeated close failed");
        return;
    }
    if (id == 6) {
        auto value = config();
        OpenEngine first(value);
        Engine second(value);
        require(!second.open(), "second engine replaced a live endpoint");
        first.events.wait([&] { return !first.events.errors.empty(); }, "live probe teardown was not observable");
        auto peer = establish(first);
        return;
    }
    if (id == 7) {
        auto value = config();
        {
            Engine stale_owner(value);
            require(stale_owner.open().has_value(), "stale-owner setup failed");
            require(stale_owner.close().has_value(), "stale-owner close failed");
        }
        Engine replacement(value);
        auto reopened = replacement.open();
        require(reopened.has_value(), "released canonical endpoint was not reusable: " + (reopened ? std::string() : reopened.error().cause));
        require(replacement.close().has_value(), "replacement close failed");
        return;
    }
    if (id == 8) {
        OpenEngine opened(config());
        auto peer = Peer::connect(opened.address);
#ifdef _WIN32
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto security = GetSecurityInfo(
            peer.stream()->native_handle(),
            SE_KERNEL_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &dacl,
            nullptr,
            &descriptor
        );
        require(security == ERROR_SUCCESS && dacl != nullptr, "Named Pipe intended-user DACL is absent");
        LocalFree(descriptor);
#else
        struct stat status{};
        require(stat(opened.address.c_str(), &status) == 0, "cannot inspect Unix socket permissions");
        require((status.st_mode & 0777) == 0600, "Unix socket mode is not 0600");
#endif
        peer.close();
        return;
    }
    if (id == 9) {
        auto value = config();
        OpenEngine opened(value);
        value.token = std::string(32, 'f');
        auto wrong = resolve_transport_address(value.endpoint_name, value.token);
        require(wrong.has_value() && *wrong != opened.address, "token did not affect canonical address");
        require(!detail::connect(*wrong), "wrong token reached the engine transport");
        return;
    }
    if (id == 10) {
        OpenEngine opened(config(2));
        auto first = establish(opened);
        auto second = establish(opened);
        require(opened.engine.close().has_value(), "orderly close failed");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.disconnected.size() == 2, "orderly close did not disconnect every session exactly once");
        require(std::ranges::all_of(opened.events.disconnected, [](const auto& event) {
            return event.reason == DisconnectReason::EngineClose;
        }), "orderly close used the wrong disconnect reason");
        return;
    }
    auto value = config();
    const auto secret = value.token;
    value.max_sessions = 0;
    Engine engine(std::move(value));
    auto opened = engine.open();
    require(!opened && opened.error().cause.find(secret) == std::string::npos, "diagnostic exposed the endpoint token");
}

inline void run_handshake_case(int id) {
    if (id == 12 || id == 13) {
        auto value = config();
        value.supported_capabilities = 0;
        value.supported_encodings = id == 12
            ? std::vector<Encoding>{Encoding::JSON, Encoding::MsgPack}
            : std::vector<Encoding>{Encoding::MsgPack, Encoding::JSON};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        require_ack(peer.handshake(fixture("handshake_valid")), id == 12 ? "ack_json" : "ack_msgpack");
        const auto view = wait_connected(opened);
        require(view.encoding == (id == 12 ? Encoding::JSON : Encoding::MsgPack), "connected view encoding differs from ACK");
        return;
    }
    if (id == 14) {
        OpenEngine opened(config());
        auto peer = Peer::connect(opened.address);
        auto bytes = fixture("handshake_valid");
        bytes.resize(15);
        require(peer.write(bytes).state == detail::IoState::Complete, "short handshake write failed");
        peer.close();
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "short handshake error missing");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.connected.empty() && opened.events.disconnected.empty(), "short handshake created a session");
        return;
    }
    if (id == 15) {
        reject_handshake("handshake_bad_magic", StatusCode::ERR_MAGIC_MISMATCH);
        return;
    }
    if (id == 16) {
        reject_handshake("handshake_bad_version", StatusCode::ERR_VERSION_MISMATCH);
        return;
    }
    if (id == 17) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        require(peer.write(fixture("handshake_encoding_unsupported")).state == detail::IoState::Complete, "unsupported handshake write failed");
        std::array<std::byte, 4> ack{};
        require(peer.read(ack).state == detail::IoState::Closed, "unsupported encoding produced an ACK");
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "unsupported encoding error missing");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.errors.back().status == StatusCode::ERR_ENCODING_UNSUPPORTED, "unsupported encoding status differs");
        return;
    }
    if (id == 18) {
        {
            OpenEngine opened(config());
            auto peer = Peer::connect(opened.address);
            static_cast<void>(peer.handshake(fixture("handshake_valid")));
        }
        auto value = config();
        value.expected_pid = process_id() + 1;
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        auto bytes = fixture("handshake_valid");
        set_u32(bytes, 8, process_id());
        require(peer.write(bytes).state == detail::IoState::Complete, "PID mismatch write failed");
        std::array<std::byte, 4> ack{};
        require(peer.read(ack).state == detail::IoState::Closed, "PID mismatch produced an ACK");
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "PID mismatch error missing");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.errors.back().status == StatusCode::ERR_PID_MISMATCH, "PID mismatch status differs");
        return;
    }
    if (id == 19) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        auto bytes = fixture("handshake_valid");
        bytes[12] = std::byte{0x83};
        require_ack(peer.handshake(bytes), "ack_json");
        return;
    }
    if (id == 20) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        require_ack(peer.handshake(fixture("handshake_cap_correlation")), "ack_cap_correlation");
        require(wait_connected(opened).capabilities == CAP_CORRELATION, "connected capability intersection differs");
        return;
    }
    if (id == 21) {
        auto value = config();
        value.supported_capabilities = 0;
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        require_ack(peer.handshake(fixture("handshake_cap_correlation")), "ack_capabilities_none");
        return;
    }
    if (id == 22) {
        auto value = config();
        value.supported_encodings = {Encoding::JSON, Encoding::MsgPack};
        OpenEngine opened(std::move(value));
        auto peer = Peer::connect(opened.address);
        auto bytes = fixture("handshake_cap_correlation");
        bytes[13] = std::byte{0x80};
        bytes[14] = std::byte{0x40};
        require_ack(peer.handshake(bytes), "ack_cap_correlation");
        return;
    }
    {
        auto hooks = std::make_shared<detail::EngineTestHooks>();
        hooks->fail_ack_write.store(true, std::memory_order_release);
        OpenEngine opened(config(), hooks);
        auto peer = Peer::connect(opened.address);
        static_cast<void>(peer.write(fixture("handshake_valid")));
        std::array<std::byte, 4> ack{};
        require(peer.read(ack).state == detail::IoState::Closed, "injected ACK failure emitted ACK bytes");
        opened.events.wait([&] { return !opened.events.errors.empty(); }, "injected ACK failure was not observable");
        std::lock_guard lock(opened.events.mutex);
        require(opened.events.errors.back().phase == ErrorPhase::AckWrite && opened.events.connected.empty(), "ACK failure created a visible session");
    }
    auto hooks = std::make_shared<detail::EngineTestHooks>();
    hooks->fail_session_write.store(true, std::memory_order_release);
    OpenEngine opened(config(), hooks);
    auto peer = Peer::connect(opened.address);
    static_cast<void>(peer.write(fixture("handshake_valid")));
    std::array<std::byte, 4> ack{};
    require(peer.read(ack).state == detail::IoState::Complete, "session-write failure did not preserve complete ACK");
    std::array<std::byte, 6> frame_header{};
    require(peer.read(frame_header).state == detail::IoState::Closed, "session-write failure emitted a session frame");
    opened.events.wait([&] { return !opened.events.errors.empty(); }, "session-write failure was not observable");
    std::lock_guard lock(opened.events.mutex);
    require(opened.events.errors.back().phase == ErrorPhase::SessionWrite && opened.events.connected.empty(), "session-write failure created a visible session");
}

}
