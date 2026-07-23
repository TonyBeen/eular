# libutp C Migration Workspace

This directory is an isolated C11 migration workspace. It is intentionally not
included by the repository root CMake build yet. The current C++ implementation
remains the behavioral baseline until a C module has equivalent tests.

The binding C11 design and code rules are in [STYLE.md](STYLE.md). They adapt
the Google C++ Style Guide to C and add transport-specific memory, hot-path,
and error-handling constraints.

The public/private error, POSIX mapping, and hierarchical logging contract is
defined in [ERRORS.md](ERRORS.md).

Only `include/utp/` is public and installed. Bounded containers, allocators,
internal error values, and log-scope helpers live under `src/internal/`; they
are implementation details and are not ABI contracts.

## Rules

- Public C objects are opaque handles.
- Every resource has one explicit create/destroy owner.
- Recoverable failures use status codes and output parameters.
- No exception-based control flow, C++ headers, or C++ runtime dependency is
  permitted in this subtree.
- Every externally supplied buffer has a pointer and length.
- Callbacks receive explicit user data and must document event-loop affinity.
- Allocation, parsing, and reassembly paths must have bounded memory limits.

## Migration Order

1. Public status codes, configuration, opaque handles, and callback contracts.
2. Small value modules: address parsing, time, packet headers, frame codecs,
   ACK ranges, and bounded containers.
3. Crypto wrappers and key schedule with explicit cleanup.
4. Socket, event-loop, memory-manager, congestion-control, and scheduler code.
5. Stream, connection, and context state machines.
6. C API parity tests, integration tests, sanitizers, and performance checks.

The C library starts at semantic version 1.0.0. Its public API is still only a
build and ABI probe and is not the final public API.

## Container Policy

Do not build a general-purpose STL replacement. Reuse the existing C red-black
tree and intrusive queue macros where they fit. New code is limited to bounded
byte buffers, fixed-capacity rings, coalescing range sets, and one intrusive
hash-table implementation.

The intrusive hash table owns only its bucket array. Each owner embeds a
`utp_hash_node` hook and supplies key hashing and match callbacks at lookup
time. CID, peer-address, replay, and pending-handshake indexes therefore share
the same table implementation without per-key container specializations.

A node may be linked into only one hash table per hook. Objects that need more
than one index embed separate hooks. Nodes cache their keyed hash so rehashing
does not inspect owner-specific keys. Every network-facing table uses a
per-context keyed hash and an explicit maximum capacity. Allocation failure
rejects only the new operation and leaves the existing table unchanged.

`utp_hash_table_init_with_buckets()` preallocates buckets before the table is
published. Its requested bucket count is rounded up to a power of two; normal
insertions remain allocation-free until a later rehash is needed. A failed
rehash keeps the old bucket array and all existing nodes valid.

## Performance And Test Gate

No container is migrated without tests and a baseline. Each implementation must
include all of the following before it is used by Context or Connection:

1. Unit tests for insert, lookup, delete, iteration, capacity limits, reset,
   and cleanup idempotence.
2. Randomized differential tests against a simple reference model, including
   at least 100,000 mixed operations and duplicate or out-of-order inputs.
3. Allocation-failure tests for every allocation or resize point. The old state
   must remain valid and no resource may leak.
4. ASan and UBSan test runs, plus fuzzing for containers fed by packet data.
5. Benchmarks for lookup/insert/delete throughput, range merging, reassembly,
   memory per entry, and allocator call counts at the configured capacity.

The first benchmark run establishes a versioned baseline with compiler, build
mode, CPU, operating system, workload, capacity, and seed recorded. Performance
results are compared only on the same benchmark environment; timing-only checks
do not run as a hard gate on shared CI hosts.

Build the optional container benchmark with:

```sh
cmake -S c -B build/c-bench -DUTP_C_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/c-bench --parallel
build/c-bench/utp_c_container_benchmark 1000000
```

The optional argument is the number of lookup and range-insert operations. The
benchmark prints the workload seed and operation counts; baseline records must
also include the compiler and flags, build mode, CPU, and operating system.
