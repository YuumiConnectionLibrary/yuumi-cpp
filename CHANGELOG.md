# Changelog

## 0.3.0 - 2026-08-03

- Replace the listener topology with one explicit platform-native dial attempt.
- Replace `open()` with `connect()` and expose the required Engine state machine.
- Receive the Go handshake before writing ACK and session assignment.
- Replace session-handle sends with current-epoch sends and single-use responders.
- Move endpoint creation, stale cleanup, permissions, and pipe security out of the engine.
- Serialize callbacks through a bounded application queue with terminal backpressure.
- Replace the listener test peer with a private Go-role listener covering EC-001 through EC-025.
- Add Debug and Release presets for Windows, Linux, and macOS plus Linux address/undefined and thread sanitizer presets.
- Rename the example from `cpp_server` to `cpp_engine`.

## 0.2.0 - 2026-07-26

- Replace the single-connection server surface with the protocol version 1 Engine API.
- Accept up to `max_sessions` concurrent pre-session or established connections.
- Isolate encoding, capabilities, heartbeat, fragments, correlation, session ID, and epoch per session.
- Use owner-only Unix sockets on Linux and macOS.
- Restore Windows transport with cancellable multi-instance byte-stream Named Pipes.
- Restrict Windows pipes to the current user SID and reject remote clients.
- Make the token-bearing canonical address mandatory and PID filtering optional.
- Preserve correlation IDs through session-scoped correlated sends.
- Validate frame and reassembled sizes before untrusted allocation or extension.
- Consume canonical vectors directly from `yuumi-spec` in CTest cases EC-001 through EC-064.
- Remove the unused Asio runtime dependency.
