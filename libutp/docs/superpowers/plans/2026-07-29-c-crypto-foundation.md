# C Crypto Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add allocation-bounded internal C11 primitives for X25519, direction-separated HKDF-SHA256 traffic keys, and AES-128/256-GCM packet payload protection.

**Architecture:** `internal/crypto.h` owns fixed-width key and key-material values while `crypto.c` is the only BoringSSL consumer. An AEAD context is initialized once and reused for packet sealing/opening; its nonce is `prefix[4] || BE64(packet_number)`. Token, resumption state, 0-RTT, handshake frame integration, and public API exposure remain outside this foundation.

**Tech Stack:** C11, bundled BoringSSL C API, Catch2, CMake, ASan/UBSan.

---

### Task 1: Link The Bundled Crypto Provider And Add Test Target

**Files:**
- Modify: `c/CMakeLists.txt`
- Create: `c/test/test_crypto.cc`

- [ ] **Step 1: Write a compile-time failing internal crypto test**

Create `test_crypto.cc`, include `internal/crypto.h`, instantiate `utp_crypto_key_pair_t`, and require a key-pair generation call to return `UTP_INTERNAL_ERROR_OK`.

- [ ] **Step 2: Build the test to verify it fails**

Run: `cmake --build /tmp/libutp-c-proto-build --target utp_c_crypto_test --parallel`

Expected: FAIL because the target/header/function do not exist.

- [ ] **Step 3: Add provider wiring**

Add the bundled `3rd/boringssl` subdirectory to the standalone C build with tests/install disabled, add `src/crypto.c` to `utp_c`, link target `crypto` privately, and register `utp_c_crypto_test` with CTest.

- [ ] **Step 4: Rebuild to verify the missing API remains the only failure**

Run: `cmake --build /tmp/libutp-c-proto-build --target utp_c_crypto_test --parallel`

Expected: FAIL on undeclared internal crypto types/functions, proving CMake linkage is ready.

### Task 2: Implement X25519 And Traffic-Key Derivation

**Files:**
- Create: `c/src/internal/crypto.h`
- Create: `c/src/crypto.c`
- Modify: `c/test/test_crypto.cc`

- [ ] **Step 1: Add failing behavioral tests**

Use two generated X25519 key pairs to derive equal shared secrets. Require zero peer public key rejection. Derive AES-128 and AES-256 traffic material with the canonical 77-byte transcript and require altered CIDs to produce different material and the two directions to differ.

- [ ] **Step 2: Run the crypto test to verify it fails**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_crypto_test`

Expected: FAIL because the crypto primitive implementation is absent.

- [ ] **Step 3: Implement the minimal primitive API**

Implement explicit key-pair clear/generate/from-private/derive functions with BoringSSL `X25519_*`, reject all-zero shared secrets, construct the canonical transcript, and use `SHA256` plus `HKDF(... EVP_sha256() ...)`. Cleanse temporary shared and expanded material on every exit path.

- [ ] **Step 4: Run the crypto test to verify it passes**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_crypto_test`

Expected: PASS.

### Task 3: Implement Packet AEAD Contexts

**Files:**
- Modify: `c/src/internal/crypto.h`
- Modify: `c/src/crypto.c`
- Modify: `c/test/test_crypto.cc`

- [ ] **Step 1: Add failing AEAD tests**

Initialize AES-256-GCM with a derived key and prefix. Seal a payload using a 20-byte AAD and packet number, open it successfully, then require failure after changing ciphertext, tag, AAD, or packet number. Require an undersized output capacity to return overflow without producing an output length.

- [ ] **Step 2: Run the crypto test to verify it fails**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_crypto_test`

Expected: FAIL because AEAD context APIs are absent.

- [ ] **Step 3: Implement fixed-nonce AEAD operations**

Allocate and initialize BoringSSL `EVP_AEAD_CTX` only in `utp_crypto_aead_init`; cleanup is idempotent. Build the 12-byte nonce locally, call `EVP_AEAD_CTX_seal/open`, reject invalid capacities and uninitialized contexts, and map provider failures to crypto encryption/decryption internal errors.

- [ ] **Step 4: Run the crypto test to verify it passes**

Run: `ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure -R utp_c_crypto_test`

Expected: PASS.

### Task 4: Verify The C Foundation

**Files:**
- Modify: `c/CMakeLists.txt`
- Modify: `docs/superpowers/plans/2026-07-29-c-crypto-foundation.md`

- [ ] **Step 1: Include new sources in the format target**

Add `src/crypto.c`, `src/internal/crypto.h`, and `test/test_crypto.cc` to `UTP_C_FORMAT_SOURCES`.

- [ ] **Step 2: Run normal verification**

Run: `cmake --build /tmp/libutp-c-proto-build --parallel && cmake --build /tmp/libutp-c-proto-build --target utp_c_format_check && ctest --test-dir /tmp/libutp-c-proto-build --output-on-failure`

Expected: build succeeds, format check passes, and all CTest tests pass.

- [ ] **Step 3: Run sanitizer verification**

Run: `cmake --build /tmp/libutp-c-sanitize-build --parallel && ctest --test-dir /tmp/libutp-c-sanitize-build --output-on-failure`

Expected: all sanitizer-instrumented tests pass with no AddressSanitizer or UndefinedBehaviorSanitizer diagnostic.
