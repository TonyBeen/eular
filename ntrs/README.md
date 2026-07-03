# NTRS

NTRS is the private NAT traversal sibling project that coexists with the standard
`stun` project.

Current state:

- `ntrs` is the formal project root and documentation home.
- Public API is exposed only through `include/ntrs/ntrs.h`.
- Internal protocol/auth/codec/io headers live under `src/ntrs` and are not public API.
- The default build produces both `libntrs.a` and the platform shared library.
- `stun` and `ntrs` intentionally coexist as two separate code trees.
- The CMake target exposed to sibling projects is `eular::ntrs`.
- New integration work should point at `../ntrs`, not `../stun`.

Scope for the current development track:

- control/auth/session
- private NAT probing
- UDP hole punching
- candidate selection and diagnostics
- KCP thin adapter only

Out of scope for the current track:

- UTP adapter
- TURN
- RTT/MTU/loss example probes as core library behavior
