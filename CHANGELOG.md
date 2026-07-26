# Changelog

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
