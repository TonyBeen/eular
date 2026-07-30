# C Send Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the C11 connection send controller required for bounded reliable delivery, using libutp protocol semantics and lsquic's C control-flow structure.

**Architecture:** `context/send_control` owns the unacknowledged, scheduled, lost, and bounded buffered packet queues. Protocol decisions come from `utp-04-reliability-ack.md` and C++ `SendControl`; packet lifecycle, queue mutation, and timer state follow the equivalent lsquic send-control structure. Congestion control and pacing are separate C modules called through narrow, allocation-free interfaces.

**Tech Stack:** C11, libevent C API timers, intrusive `TAILQ`, `utp_packet_out_pool`, Catch2, BoringSSL only through the existing crypto adapter.

---

### Task 1: Complete Send-Control State and Packet Scheduling

**Files:**
- Modify: `c/src/context/send_control.{c,h}`
- Modify: `c/src/proto/packet_out.{c,h}`
- Test: `c/test/test_send_control.cc`

- [ ] Write failing tests for FIFO scheduling, explicit `TRACK_ON_SEND`, capacity limits, and dequeue state cleanup.
- [ ] Add bounded scheduled and lost `TAILQ`s, counters, packet-number allocation, and lifecycle flags to `utp_send_control_t`.
- [ ] Implement `schedule_packet`, `next_packet_to_send`, and `packet_sent`; only `packet_sent` inserts a tracked packet into the ACK ledger.
- [ ] Run `utp_c_send_control_test` and the format check.

### Task 2: Loss Detection and Retransmission Preparation

**Files:**
- Modify: `c/src/context/send_control.{c,h}`
- Test: `c/test/test_send_control.cc`

- [ ] Write failing tests for FACK loss, send-time loss, duplicate-loss suppression, non-retransmittable packet discard, and MTU-probe discard.
- [ ] Implement the C++/lsquic-style unacked scan: FACK threshold and send-time conditions only for packet numbers not above `largest_acked`.
- [ ] Move retransmittable lost packets to the lost queue with `UTP_PO_LOST`, `UTP_PO_LOSS_RECORDED`, and `UTP_PO_RESET_PACKNO`; return non-retransmittable and MTU probes for caller release.
- [ ] Run the focused test, all CTest targets, format check, and ASan/UBSan target.

### Task 3: Retransmission Timing and Expiration

**Files:**
- Modify: `c/src/context/send_control.{c,h}`
- Test: `c/test/test_send_control.cc`

- [ ] Write failing tests for handshake, loss, TLP, and RTO mode priority; 60-second cap; 200-ms RTO floor; bounded exponential backoff.
- [ ] Implement pure timing calculations and `on_retransmission_timeout`, which moves the appropriate bounded packet subset to the lost queue.
- [ ] Add `utp_event_t` integration only after pure timeout tests pass; Context owns event registration and invokes the controller with monotonic time.
- [ ] Run focused and full tests, format, and sanitizer verification.

### Task 4: Pacer and Congestion Interfaces

**Files:**
- Create: `c/src/congestion/congestion.{c,h}`
- Create: `c/src/congestion/pacer.{c,h}`
- Modify: `c/src/context/send_control.{c,h}`
- Test: `c/test/test_congestion.cc`, `c/test/test_send_control.cc`

- [ ] Write failing deterministic tests for cwnd admission, packet accounting, loss callback ordering, and pacing deadlines.
- [ ] Port the small C-compatible pacing mechanism from lsquic, then define a C vtable for BBR/CUBIC packet-sent/ACK/loss/timeout events.
- [ ] Port the selected algorithms only after their dependency graph is bounded and each algorithm has deterministic vectors; never introduce dynamic allocation in ACK/send paths.
- [ ] Run all unit tests, deterministic simulations, sanitizer build, and benchmark baseline.

### Task 5: Connection Integration

**Files:**
- Create: `c/src/connection/connection.{c,h}`
- Modify: `c/src/context/context.{c,h}`
- Test: `c/test/test_connection.cc`

- [ ] Write failing loopback tests for Initial-to-HandshakeDone, send-controller callback order, retransmission after dropped datagram, and clean packet-pool return.
- [ ] Connect send-control output to packet encoding, UDP send completion, connection timers, and CID demux without adding allocation to established receive/send paths.
- [ ] Only after packet dispatch exists, add Context UDP-read event registration; unknown CID packets must not be consumed by a placeholder callback.
- [ ] Run all CTest targets, network simulation tests, ASan/UBSan, formatting, and benchmark checks.

## Self-Review

- Protocol behavior maps to `utp-04-reliability-ack.md`; no QUIC packet-number spaces, header protection, Retry, or HTTP/3 semantics are copied from lsquic.
- All packet collections remain bounded by connection configuration and use existing packet-pool ownership.
- Packet parsing, ACK processing, loss scanning, and retransmission preparation are allocation-free.
- Every mutation has a deterministic Catch2 regression before implementation, and connection integration waits for a real CID demux consumer.
