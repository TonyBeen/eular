# C11 Design And Code Rules

This subtree follows the Google C++ Style Guide where it applies to C, adapted
for strict C11. C has no separate Google style guide; this document makes the
adaptation explicit and is normative for `c/` production code and tests.

## Mechanical Rules

- Source must be formatted with the local `.clang-format`, whose base style is
  Google. The column limit is 80 and indentation is two spaces.
- All production code must compile as C11 with extensions disabled and the
  warning set configured in `CMakeLists.txt`; warnings are errors.
- Use `snake_case` for functions and variables, `_t` only for public types,
  and the `UTP_` prefix for public macros and enum values. Public headers use
  stable, path-derived include guards.
- Headers include what they use. Include project headers after C standard
  headers. Do not use VLAs, compiler extensions, implicit conversions that lose
  data, mutable global state, recursion in packet paths, or macro arguments
  with side effects.

## API And Ownership

- A public operation has one explicit owner for every allocation and one
  idempotent cleanup path. Initialization failure leaves its output object
  cleared and safe to clean up.
- Production code obtains memory only through `utp_allocator_t`. Benchmarks
  and test fixtures may use libc allocation directly.
- Every externally supplied buffer is `(pointer, length)`; every output buffer
  has an explicit capacity. Never rely on NUL termination for network data.
- Recoverable public errors return `utp_status_t`; private code returns the
  internal error type. No `abort`, assertions, or logging of secret or packet
  payload data is permitted in production paths.
- Public APIs must document ownership, object lifetime, callback reentrancy,
  thread or event-loop affinity, and all resource limits.

## Resource And Performance Rules

- Packet parsing, ACK processing, and established-connection send/receive
  paths must not allocate. Their memory is bounded at Context or Connection
  creation.
- A hash table may rehash only as an admission or management operation. Failed
  rehash must preserve the old table and existing connections; it can reject
  only the new insertion.
- Every untrusted-cardinality collection has a configured maximum. Check
  integer overflow before size arithmetic, allocation, indexing, and counter
  advancement.
- Any new hot-path container needs a benchmark and a deterministic regression
  test. Measure on a fixed environment before accepting a performance claim.

## Design And Review Rules

- Prefer small interfaces with one responsibility. Do not add a general
  container, abstraction, or callback layer without at least two concrete
  protocol consumers.
- Keep policy in Context/Connection and mechanism in small testable modules.
  Protocol state changes must be explicit enums with checked transitions.
- Changes to parsing, allocation, crypto, retransmission, or indexing require
  normal tests, randomized/differential tests where applicable, allocation
  failure coverage, and UBSan/ASan runs in a supported environment.
- Reviewers reject changes that weaken bounds, silently drop errors, mutate
  existing connection state after a failed admission, or make the packet path
  depend on unbounded work.
- Public status values must not be raw `errno` values. Capture POSIX errors at
  the operating-system boundary using the internal error facility, then map
  them at the public boundary. Errors propagate; logs do not.
- Logs use stack-scoped hierarchical tags. Each tag is at most 256 bytes and
  must identify an operational layer without recording secrets or payloads.
