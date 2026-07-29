# Error And Logging Contract

## Error Domains

Public functions return only negative `utp_status_t` values from
`include/utp/status.h` (`0` is success). Generic meanings such as invalid
arguments, missing objects, memory exhaustion, timeout, and invalid state have
exactly one shared status code. Errors that need distinct handling use stable
negative ranges: socket `-0x0020..-0x003f`, stream `-0x0040..-0x005f`, frame
`-0x0060..-0x007f`, crypto `-0x0080..-0x009f`, context
`-0x00a0..-0x00bf`, connection `-0x00c0..-0x00df`, and rendezvous
`-0x00e0..-0x00ff`.

Callers can inspect a value directly or compare against named codes such as
`UTP_STATUS_SOCKET_READ` and `UTP_STATUS_CONNECTION_HANDSHAKE`. Status values
are portable and must not expose platform-specific `errno` values.

Private code returns `utp_internal_error_t` from `src/internal/error.h`. It is
a value that can cross internal function boundaries without allocation. It has
separate internal and POSIX facilities. `utp_internal_error_from_errno()`
captures `errno` immediately at the system-call boundary.
`utp_internal_error_to_status()` maps generic meanings only. A public boundary
that knows the failed operation returns its specific socket, context,
connection, stream, frame, crypto, or rendezvous status directly.

Protocol, cryptographic, state-machine, capacity, and parser failures use the
internal facility, never synthetic POSIX values. The original internal value is
not part of the installed ABI. This keeps public behavior portable while still
giving local diagnostics the exact operating-system cause.

## Logging

An error code is return data. A log message is a local side effect. Error
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
the log call. The logger formats the complete message before calling
`utp_log_sink_fn(level, message)`. The message is valid only during the
synchronous callback and is at most 2048 bytes, excluding its null terminator.

Error logs use the following format:

```text
<tag> <message>: status=<public_status>, errno=<posix_errno>
```

The tag and `errno` portion are omitted when unavailable.

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
