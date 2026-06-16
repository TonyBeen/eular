# NTRS

NTRS is the private NAT traversal sibling project that coexists with the standard
`stun` project.

Current state:

- `ntrs` is the formal project root and documentation home.
- `ntrs` now carries its own copied implementation under `ntrs/include`, `ntrs/src`,
  `ntrs/examples`, and `ntrs/test`.
- `stun` and `ntrs` intentionally coexist as two separate code trees.
- The target exposed to sibling projects is `ntrs`.
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
