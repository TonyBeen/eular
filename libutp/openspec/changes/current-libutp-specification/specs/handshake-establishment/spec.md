## ADDED Requirements

### Requirement: Active handshake reaches connected state only after peer acceptance
The libutp transport SHALL complete an active connection only after the peer handshake has been accepted and the connection has been promoted to the connected state.

#### Scenario: Active connect promotes only on successful handshake
- **WHEN** an active connect attempt exchanges handshake packets and the peer accepts the handshake
- **THEN** the endpoint MUST promote the connection to connected and expose it as an established connection
