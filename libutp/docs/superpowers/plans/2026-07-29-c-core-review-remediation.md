# C Core Review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Restore C/C++ wire compatibility, remove protocol-parser duplication, and eliminate the reviewed cross-platform and documentation defects without expanding the public API.

**Architecture:** Keep packet parsing allocation-free and failure-atomic. Move ACK wire-size constants to the ACK internal interface, make padding decoding obey the C++ wire contract, and centralize IPv6 unspecified-address classification in the address mechanism. Defer event-loop allocator routing and packet frame-metadata caching because both require an explicit ownership model.

**Tech Stack:** C11, libevent C API, Catch2, CMake, clang-format.

---

### Task 1: Align Padding Wire Compatibility

**Files:**
- Modify: `c/test/test_proto.cc`
- Modify: `c/src/frame.c`

- [x] **Step 1: Write the failing test**

Change the padding test to set one payload byte to `1` and require `utp_frame_padding_decode()` to return `UTP_INTERNAL_ERROR_OK` with the encoded length.

- [x] **Step 2: Run the focused test to verify it fails**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: FAIL because the decoder returns `UTP_INTERNAL_ERROR_PROTOCOL` for non-zero padding.

- [x] **Step 3: Write the minimal implementation**

Remove the padding-payload byte loop from `utp_frame_padding_decode()`. Retain header type, length arithmetic, and bounds validation.

- [x] **Step 4: Run the focused test to verify it passes**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: PASS.

### Task 2: Make ACK Range Decoding Single-Pass And Atomic

**Files:**
- Modify: `c/src/internal/ack.h`
- Modify: `c/src/ack.c`
- Test: `c/test/test_proto.cc`

- [x] **Step 1: Extend malformed-range coverage**

Add a two-range ACK whose second range has a zero length. Require a protocol error and require all caller-owned ACK fields and range entries to remain unchanged.

- [x] **Step 2: Run the focused test to establish the invariant**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: PASS on the current implementation; the test documents the existing failure-atomic contract before refactoring.

- [x] **Step 3: Write the implementation**

Define `UTP_ACK_FRAME_HEADER_SIZE` and `UTP_ACK_FRAME_RANGE_SIZE` in `internal/ack.h`. Decode each extra range once into bounded local `uint32_t` gap/length arrays, validate while reading, then derive and commit the caller ranges only after full validation.

- [x] **Step 4: Run the focused test after refactoring**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: PASS, including the unchanged-output assertion.

### Task 3: Centralize Shared Wire And Address Mechanisms

**Files:**
- Modify: `c/src/frame.c`
- Modify: `c/src/address.c`
- Modify: `c/src/internal/address.h`
- Modify: `c/src/udp.c`
- Test: `c/test/test_proto.cc`

- [x] **Step 1: Add address predicate coverage**

Add tests for IPv6 `::`, `::1`, and an IPv4 address. Require only `::` to be classified as an unspecified IPv6 address.

- [x] **Step 2: Run the focused test to verify it fails**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: FAIL until the new predicate is declared and implemented.

- [x] **Step 3: Write the minimal implementation**

Add `utp_address_is_unspecified_ipv6()` to the internal address interface and implement it with a fixed local zero byte array. Replace both UDP compound literals with this helper. Include `internal/ack.h` in `frame.c` and delete its duplicate ACK size macros.

- [x] **Step 4: Run the focused test to verify it passes**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_proto_test`

Expected: PASS.

### Task 4: Remove Premature Policy And Correct Documentation

**Files:**
- Modify: `c/src/internal/frame.h`
- Modify: `c/src/internal/proto.h`
- Modify: `c/ERRORS.md`

- [x] **Step 1: Remove unused policy declarations**

Delete `UTP_FRAME_RETRANSMISSION_MASK`, `UTP_DEFAULT_ACK_THRESHOLD`, `UTP_DEFAULT_MAX_ACK_DELAY_MS`, and `UTP_DEFAULT_REORDER_THRESHOLD`; no C connection/reliability owner exists yet.

- [x] **Step 2: Correct the logging contract**

Change the documented formatted-message maximum in `c/ERRORS.md` from 1024 to 2048 bytes, excluding the null terminator.

- [x] **Step 3: Build and run all tests**

Run: `cmake --build /tmp/libutp-c-proto-build --parallel && ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure`

Expected: build succeeds and all CTest tests pass in an environment allowed to bind loopback UDP sockets.

### Task 5: Record Deferred Architectural Review Items

**Files:**
- Modify: `c/STYLE.md`

- [x] **Step 1: Document the third-party allocation exception**

State that `libevent` owns allocations behind its process-global allocator hook; Context-specific `utp_allocator_t` routing must not be emulated with mutable global state. A future Context design must either accept an externally-owned `event_base` or establish one explicitly process-wide allocator before libevent initialization.

- [x] **Step 2: Preserve the selected logging API**

Keep `utp_log_sink_fn(level, message)` without callback user data. Context and connection attribution is carried by the hierarchical formatted tag, matching the established API decision.

- [x] **Step 3: Retain the event-loop adapter as approved foundation**

Do not delete `event_loop` solely because current consumers are tests; it is the selected C/libevent mechanism for the forthcoming Context implementation.
