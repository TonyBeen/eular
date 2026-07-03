## Requirement To Test Traceability

This note links the current OpenSpec baseline to the regression tests that exercise each requirement area most directly.

## ack-trigger-policy

- Delayed ACK timer flush: `test/test_ack_behavior.cc` case `Ack timer sends delayed ACK without follow-up packets`

## ack-frequency-synchronization

- AckFrequency receiver and sender threshold propagation: `test/test_ack_behavior.cc` case `Ack reordering threshold updates send side and triggers immediate ACK on gap`
- ACK piggyback and negotiated ACK behavior: `test/test_ack_behavior.cc` and `test/test_context_integration.cc`

## loss-retransmission-cycle

- Loss detection and retransmission behavior: `test/test_packet_out.cc`, `test/test_ack_behavior.cc`, and integration paths in `test/test_context_integration.cc`

## path-challenge-lifecycle

- Address change enters validating state: `test/test_network_path.cc` case `address change triggers validating`
- Matching challenge/response validates path: `test/test_network_path.cc` case `challenge-response validates path`
- Retry exhaustion fails path: `test/test_network_path.cc` case `timeout can move path to failed`
- Mismatched response stays validating: `test/test_network_path.cc` case `mismatched response should be ignored`
- Context-level counters: `test/test_context_integration.cc` case `path validation counters track started/succeeded/failed`

## path-amplification-limits

- Validating-path send gating and traffic limitation: `test/test_network_path.cc` and context integration path-validation scenarios in `test/test_context_integration.cc`

## zero-rtt-token-gating

- Valid token acceptance and metadata recovery: `test/test_context_zero_rtt.cc` case `validateZeroRttTicket accepts valid bound token`
- Peer mismatch rejection: `test/test_context_zero_rtt.cc` case `validateZeroRttTicket rejects peer mismatch`
- Token expiry rejection: `test/test_context_zero_rtt.cc` case `validateZeroRttTicket rejects expired token`

## zero-rtt-replay-defense

- Replay window rejection and purge: `test/test_context_zero_rtt.cc` case `rememberZeroRttNonce rejects duplicates within replay window`

## zero-rtt-decision-signaling

- Rejected 0-RTT decision callback: `test/test_context_integration.cc` case `0-RTT rejected ticket closes client without delivering early stream`
- Accepted and replay-rejected 0-RTT decisions: `test/test_context_integration.cc` case `0-RTT replay is rejected and counted`

## passive-accept-control

- Passive pending accept and callback gating: `test/test_context_integration.cc` passive connect/accept scenarios

## connection-close-convergence

- Remote close convergence and callback delivery: `test/test_context_integration.cc` case `remote ConnectionClose converges to OnConnectionClosed callback`

## stream-discovery-and-lookup

- Stream lookup semantics for existing and unknown IDs: `test/test_connection_stream_layer.cc`

## stream-id-admission

- Role-bit validation and per-type admission limits: `test/test_connection_stream_layer.cc`

## stream-read-fin-reset

- In-order readability, gap handling, FIN/RESET behavior: `test/test_stream_impl.cc`

## Gaps Closed In This Session

- `connection-close-convergence` has direct regression evidence for `OnConnectionClosed` delivery after remote `ConnectionClose` drains.
