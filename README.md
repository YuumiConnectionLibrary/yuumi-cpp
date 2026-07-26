# yuumi-cpp

[![CI](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml)

> C++23 engine SDK for the [Yuumi IPC protocol](https://github.com/YuumiConnectionLibrary/yuumi-spec).

Yuumi connects a Go shell to a C++ logic engine through platform-native local IPC. The C++ SDK opens the endpoint, accepts isolated sessions, owns protocol Control traffic, and sends application output to one explicit session at a time. It does not expose a public client, process launcher, restart policy, or application semantics.

## Requirements

- CMake 3.28+
- C++23 compiler: MSVC 19.38+, Clang 17+, or a compatible newer release
- vcpkg
- A sibling `yuumi-spec` checkout, or `YUUMI_SPEC_DIR` pointing to one, when tests are enabled

The only runtime codec dependency is `nlohmann-json`. Windows transport and security use Win32 directly.

## Build and test

```bash
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug-build
ctest --preset linux-clang-debug-test --output-on-failure
```

Use `windows-msvc-debug` or `macos-clang-debug` and their matching build/test presets on the other supported platforms. The test configuration reads canonical `.bin` fixtures directly from `yuumi-spec/test-vectors`; it never copies them into this repository.

## Engine configuration

```cpp
#include <yuumi/bridge.hpp>

yuumi::EngineConfig config;
config.endpoint_name = "pricing";
config.token = "0123456789abcdef0123456789abcdef";
config.max_sessions = 4;

yuumi::Engine engine(std::move(config));

engine.on_message([&engine](const yuumi::MessageEvent& message) {
    const yuumi::Json response{{"ok", true}};
    if (message.correlation_id) {
        engine.send_correlated(
            message.session,
            yuumi::Channel::Data,
            *message.correlation_id,
            response
        );
    } else {
        engine.send(message.session, yuumi::Channel::Data, response);
    }
});

auto opened = engine.open();
if (!opened) {
    return 1;
}
```

`open()` validates the complete configuration, secures the endpoint, starts accepting, and returns without waiting for a client. `close()` stops admission first, closes every session, waits for disconnect notifications, and is safe to call repeatedly.

Required configuration:

| Field | Behaviour |
|---|---|
| `endpoint_name` | 1–32 ASCII characters matching `[A-Za-z0-9][A-Za-z0-9_-]{0,31}` |
| `token` | Exactly 32 lowercase hexadecimal characters; included in the canonical address |

Important defaults:

| Field | Default |
|---|---|
| `max_sessions` | `1` |
| `supported_encodings` | MessagePack, then JSON |
| `supported_capabilities` | `CAP_CORRELATION` |
| heartbeat | 30 seconds, three missed intervals |
| fragmentation | 15-second timeout, 16 active sequences per session |

`expected_pid` is optional. When absent, PID filtering is disabled. When present, the wire PID must match the configured value and any trustworthy peer PID exposed by the platform. Token-bearing addresses and OS permissions remain the primary controls.

## Sessions and sends

Each connected event exposes an immutable encoding, capability mask, and `SessionHandle`. The handle contains both `session_id` and `epoch`; a handle from an earlier connection cannot address a replacement session.

Applications may send only:

- `Channel::Log`
- `Channel::Data`

Control traffic and client-to-engine `Channel::Command` are rejected by the public send surface. A correlated send additionally requires `CAP_CORRELATION` in that session.

Sequential calls for the same session are serialized. Concurrent calls follow write-mutex acquisition order. A session failure never redirects output to or closes another session.

## Event execution

For one session, callbacks start in this order:

```text
session connected
zero or more message/error callbacks
session disconnected
```

Callbacks for one session are serialized. Callbacks for different sessions may run concurrently on their session or maintenance workers, so shared application state needs synchronization. Handlers must not throw. Because `close()` waits for callback completion, synchronous close from inside a callback is rejected; schedule it on the application lifecycle thread.

## Platform transport and security

| Platform | Transport | Security |
|---|---|---|
| Linux/macOS | Unix domain stream socket under the OS temporary directory | Socket node mode `0600` |
| Windows | Byte-stream Named Pipe with multiple instances | Current-user SID ACL and `PIPE_REJECT_REMOTE_CLIENTS` |

Canonical addresses are:

```text
Linux/macOS: <os_temp_dir>/yuumi-<endpoint_name>-<token>.sock
Windows:     \\.\pipe\yuumi-<endpoint_name>-<token>
```

The engine probes by connecting before replacing an endpoint. A live or busy owner is never removed; a refused stale Unix socket is unlinked before bind.

## Conformance

CTest registers `yuumi_EC-001` through `yuumi_EC-064`, matching `ENGINE_CONFORMANCE.md`. The private test peer can send exact bytes and inspect transport controls, but remains test-only and is not a public non-Go client SDK.

## Compatibility alias

`ServerBridge` and `Bridge` are aliases for `Engine`. The old `start(pipe_name, expected_pid)` and sessionless `send(payload, channel)` signatures were removed because they cannot supply the mandatory token or target one isolated session safely.

## Issues

Protocol questions belong in [yuumi-spec](https://github.com/YuumiConnectionLibrary/yuumi-spec/issues). C++ SDK defects belong in this repository.
