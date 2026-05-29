# yuumi-cpp

[![CI](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/YuumiConnectionLibrary/yuumi-cpp/actions/workflows/ci.yml)

> C++23 server-side SDK for the [Yuumi IPC protocol](https://github.com/YuumiConnectionLibrary/yuumi-spec).

## Requirements

- CMake 3.28+
- C++23 compiler (MSVC 19.38+, GCC 13+, Clang 17+)
- [vcpkg](https://github.com/microsoft/vcpkg)

## Install

```bash
git clone --recurse-submodules https://github.com/YuumiConnectionLibrary/yuumi-cpp
```

```bash
# Linux / Clang
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug-build

# macOS
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug-build

# Windows (PowerShell)
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug-build
```

If `VCPKG_ROOT` is set in your environment, CMake picks it up automatically.

## Quick start

```cpp
#include <yuumi/bridge.hpp>

yuumi::ServerBridge bridge;

bridge.on_message([](const yuumi::Json& payload, yuumi::Channel ch) {
    // handle incoming frame from Go client
});

bridge.on_error([](yuumi::Error err) {
    // handle transport / protocol errors
});

// Start listening — blocks until the first client connects and completes handshake
auto result = bridge.start("my-service", 0 /* expected PID, 0 = any */);
if (!result) {
    return 1;
}

// Send a frame back to the client
bridge.send({{"status", "ready"}}, yuumi::Channel::Command);
```

## API reference

| Symbol | Description |
|---|---|
| `ServerBridge::start(pipe_name, expected_pid)` | Bind socket, accept client, validate handshake |
| `ServerBridge::send(json, channel)` | Enqueue and send a data frame |
| `ServerBridge::on_message(fn)` | Register callback for incoming data frames |
| `ServerBridge::on_error(fn)` | Register callback for transport / protocol errors |
| `ServerBridge::stop()` | Graceful shutdown — closes socket, joins I/O threads |
| `resolve_transport_address(pipe_name)` | Resolve `<tempdir>/<pipe_name>.sock` path |
| `Protocol::encode(json, channel, encoding)` | Build a framed byte packet |
| `Protocol::decode(buffer, encoding)` | Deserialize a payload buffer |
| `Diagnostic::log / error / success` | Structured stdout logging |

## Channels

| Value | Name | Direction | Purpose |
|---|---|---|---|
| `Channel::Control` | `0x00` | Bidirectional | Heartbeat, lifecycle |
| `Channel::Command` | `0x01` | Client → Server | Commands, requests |
| `Channel::Log` | `0x02` | Server → Client | Log output |
| `Channel::Data` | `0x03` | Bidirectional | Application payload |

## Dependencies (via vcpkg)

- [asio](https://github.com/chriskohlhoff/asio) — async I/O
- [nlohmann-json](https://github.com/nlohmann/json) — JSON + MsgPack encoding

## Wire protocol

See [yuumi-spec](https://github.com/YuumiConnectionLibrary/yuumi-spec) for the canonical wire format and conformance test vectors.

## Issues

Questions or problems? [Open an issue](https://github.com/YuumiConnectionLibrary/yuumi-cpp/issues).
