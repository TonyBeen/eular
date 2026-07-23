# Error And Logging Contract

## Error Domains

Public functions return only `utp_status_t` from `include/utp/status.h`. These
values are stable, portable categories such as `UTP_STATUS_PROTOCOL` or
`UTP_STATUS_TIMEOUT`; they are not POSIX numbers and must not expose a platform
specific `errno` as the public API result.

Private code returns `utp_internal_error_t` from `src/internal/error.h`. It is
a value that can cross internal function boundaries without allocation. It has
separate internal and POSIX facilities. `utp_internal_error_from_errno()`
captures `errno` immediately at the system-call boundary, preserving it until
the public boundary maps it with `utp_internal_error_to_status()`.

Protocol, cryptographic, state-machine, capacity, and parser failures use the
internal facility, never synthetic POSIX values. The original internal value is
not part of the installed ABI. This keeps public behavior portable while still
giving local diagnostics the exact operating-system cause.

## Logging

An error code is return data. A log event is local side effect data. Error
values never contain log messages, tags, pointers, or connection identifiers.
The first layer that both detects the error and has the relevant operational
scope emits one log event; callers propagate the error without logging it
again. Generic bounded containers therefore return errors only. Their Context,
Connection, socket, or frame-decoder caller provides the diagnostic scope.

`utp_log_tag_t` is a fixed-size, owned diagnostic prefix. Each Context,
Connection, and Stream initializes its own tag by copying its parent's complete
tag and appending one bracketed fragment. A tag is at most 256 bytes and adds
no heap allocation. `utp_internal_log_error()` therefore only references an
already completed tag; it never traverses or dynamically assembles hierarchy at
the log call. The callback must not retain the event's pointers.

Examples:

```text
[connection scid 21313]
[connection scid 21313][send_control]
[context 1][connection scid 21313][send_control]
[utp_hash_table]
```

Single-context deployments omit the context tag. Multi-context deployments add
a monotonically assigned context identifier at the root. Tags must never
contain keys, packet
plaintext, shared secrets, or unbounded peer-controlled data.
