## ADDED Requirements

### Requirement: Close processing converges away from connected lifecycle
The connection lifecycle SHALL expose an explicit close path. A local close request or received connection-close frame MUST move the connection toward shutdown, and the implementation MUST eventually stop treating that peer association as connected.

#### Scenario: Remote close converges to shutdown path
- **WHEN** a connected endpoint receives a valid connection-close indication from its peer
- **THEN** the implementation MUST stop treating the association as active connected traffic and converge the connection toward closure handling
