## Baseline Audit

This audit checks the current public API, connection state flow, and regression coverage against the decomposed OpenSpec capabilities under connection and stream domains.

## Connection Lifecycle Evidence

### Public API Surface

- `Context::setOnConnected`, `setOnConnectError`, `setOnNewConnection`, and `setOnConnectionClosed` expose the callback surface required by the baseline.
- `Context::accept`, `connect`, `connect0Rtt`, and `connect0RttWithState` define the active and passive handshake entry points.

### Runtime Evidence

- Passive pending handshake promotion is exercised by `OnNewConnection allow completes passive handshake` and `OnNewConnection may accept immediately`.
- Passive rejection leading to client-side close reception is exercised by `OnNewConnection reject returns connection close to client`.
- Remote close convergence into `OnConnectionClosed` is covered by `remote ConnectionClose converges to OnConnectionClosed callback`.
- `ContextImpl::handleConnectionState` distinguishes handshake failure from established connection closure and reports either `OnConnectError` or `OnConnectionClosed` accordingly.

### Audit Result

No functional mismatch was found between the current connection-domain baseline requirements and the observed public API plus regression behavior.

## Stream Transport Evidence

### Public API Surface

- `Connection::createStream` and `getStream` provide stream creation and lookup.
- `Stream` exposes read, write, zero-copy acquire/consume, close, reset, and readable/writable callbacks.

### Runtime Evidence

- `ConnectionImpl: getStream returns created stream pointer` verifies lookup semantics for existing and unknown stream IDs.
- `ConnectionImpl: setOnIncomingStream only fires for peer-created streams` verifies peer-created stream callback semantics.
- `ConnectionImpl: ingress stream gate checks role and per-type limits` verifies role-bit validation and per-type quota rejection.
- `ConnectionImpl: streamCount and creatableStreamCount support stream type` verifies that only locally initiated active streams consume creation quota.
- `StreamImpl: onFrame pushes data and read consumes`, `out-of-order frame is rejected for now`, `peer fin returns EOF on empty read`, and `reset frame notifies upper layer` cover in-order readability, gap handling, FIN, and reset outcomes.

### Audit Result

No functional mismatch was found between the current stream-domain baseline requirements and the observed public API plus regression behavior.

## Mismatch Review

- No baseline mismatch requiring a follow-up OpenSpec change was identified during this audit.
- If future implementation review discovers divergence from these requirements, create a new follow-up change instead of revising the baseline silently.
