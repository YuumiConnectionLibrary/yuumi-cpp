# yuumi-cpp

[![CI](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml)

> C++23 engine SDK for the [Yuumi IPC protocol](https://github.com/YuumiConnectionLibrary/yuumi-spec).

Yuumi connects a Go shell to a C++ logic engine through platform-native local IPC. The Go client owns the endpoint and listens; the C++ engine performs one explicit dial and one handshake attempt. The SDK does not expose a public listener or client, create or clean endpoints, launch processes, retry connections, or impose an application payload schema.

## Requirements

- CMake 3.28+
- C++23 compiler: MSVC 19.38+, Clang 17+, or a compatible newer release
- vcpkg
- A sibling `yuumi-spec` checkout, or `YUUMI_SPEC_DIR` pointing to one, when tests are enabled

The only runtime codec dependency is `nlohmann-json`. Transport uses the platform API directly.

## Build and test

```bash
cmake --preset linux-clang-debug -DYUUMI_SPEC_DIR=../yuumi-spec
cmake --build --preset linux-clang-debug-build
ctest --preset linux-clang-debug-test --output-on-failure
```

Replace `debug` with `release` for optimized verification. Equivalent `windows-msvc-*` and `macos-clang-*` presets run the same EC-001 through EC-025 cases. Linux also provides `linux-clang-address` for AddressSanitizer plus UndefinedBehaviorSanitizer and `linux-clang-thread` for ThreadSanitizer.

On WSL, the repository can provision its pinned vcpkg checkout without sudo and
run conformance plus the real Go-to-C++ integration cell:

```bash
bash scripts/verify-wsl.sh
```

The checkout lives in the ignored `vcpkg/` directory. The script rejects an
existing checkout at a different commit instead of changing it implicitly.

## Engine configuration

```cpp
#include <yuumi/bridge.hpp>

yuumi::EngineConfig config;
config.endpoint_name = "pricing";
config.token = "0123456789abcdef0123456789abcdef";

yuumi::Engine engine(std::move(config));
engine.on_message([&engine](const yuumi::MessageEvent& message) {
    const yuumi::Json response{{"ok", true}};
    const auto sent = message.responder
        ? message.responder->respond(response)
        : engine.send(yuumi::Channel::Data, response);
    if (!sent) {
        // Route sent.error() through the application's error policy.
    }
});

auto connected = engine.connect();
if (!connected) {
    return 1;
}
```

`connect()` validates a frozen configuration snapshot, derives the canonical address, performs one bounded dial, receives and validates the 16-byte Go handshake, writes ACK followed by session assignment, and returns only when the session is usable. It never retries. After a terminal disconnect returns the engine to `Idle`, reconnection requires another explicit `connect()`.

`close()` is valid in every state, cancels a pending attempt or active session, unblocks transport and dispatcher waits, joins owned workers, and is idempotent.

Important defaults:

| Field | Default |
|---|---|
| `supported_encodings` | MessagePack, then JSON |
| `supported_capabilities` | `CAP_CORRELATION` |
| `connect_timeout` | 10 seconds |
| `application_queue_capacity` | 64 application events |
| heartbeat | 30 seconds, three missed intervals |
| fragmentation | 15-second timeout, 16 active sequences |

`expected_go_pid` is an optional additional check. When present, it must match the handshake PID and any trustworthy peer PID exposed by the platform.

## Sessions, sends, and events

The immutable `SessionView` contains `session_id`, local `epoch`, negotiated encoding, and capabilities. Public `send()` targets the current epoch and accepts only `Channel::Log` and `Channel::Data`. Correlated inbound messages carry a single-use, epoch-bound `Responder` that always replies on `Data` with the original correlation ID.

Callbacks for one epoch are ordered and never overlap:

```text
connected
zero or more message, heartbeat, or error events
disconnected
```

Application delivery runs on a serial dispatcher separate from IPC. When its bounded capacity is exhausted, accepted events drain in order, then a reserved backpressure error and the terminal disconnected event are delivered.

## Platform transport and ownership

| Platform | Engine transport | Endpoint owner |
|---|---|---|
| Linux/macOS | Unix domain byte stream at the canonical hashed temporary path | Go client |
| Windows | Byte-stream Named Pipe at the canonical token-bearing name | Go client |

The C++ engine never binds, listens, accepts, probes liveness, removes stale endpoints, changes Unix modes, or owns Named Pipe ACLs. The private testkit contains a Go-role listener solely to execute the shared conformance contract and is not installed or published.

## Example

Build target `yuumi_cpp_engine` from `examples/cpp_engine/main.cpp`, then start it with values supplied by the Go application:

```text
cpp_engine <endpoint_name> <token> [expected_go_pid]
```

Process lifecycle and restart policy remain application responsibilities.

## Verification

After configuring a supported CMake preset, build and run CTest:

    cmake --build --preset <build-preset>
    ctest --preset <test-preset> --output-on-failure

The standalone CTest run executes the 25 canonical Engine cases. To add the
real Go-to-C++ cell, configure with YUUMI_BUILD_INTEROP_TESTS=ON, then build and
run the same CTest preset. This adds yuumi_go_integration, which starts the
private yuumi_interop_engine fixture and delegates the common scenarios to the
tagged Go driver. A missing sibling Yuumi checkout is an error. The fixture is
a test target and is not installed.

## Issues

Protocol questions belong in [yuumi-spec](https://github.com/YuumiConnectionLibrary/yuumi-spec/issues). C++ SDK defects belong in this repository.

The required CI matrix, sanitizer coverage, immutable spec pin, artifacts,
timeout, cleanup, and local equivalents are documented in [`CI.md`](CI.md).
