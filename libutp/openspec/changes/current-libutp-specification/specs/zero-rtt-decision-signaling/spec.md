## ADDED Requirements

### Requirement: Session material export and 0-RTT decision outcomes are observable
Context and connection APIs SHALL expose session resumption material and 0-RTT decisions. A connected connection MUST be able to export session material, and context MUST report whether 0-RTT was accepted or rejected.

#### Scenario: Session material export is available post-connect
- **WHEN** a connected connection has prepared fresh resumption material
- **THEN** connection MUST allow exporting session token or opaque resumption state for later use

#### Scenario: Rejected 0-RTT emits reasoned decision event
- **WHEN** a 0-RTT attempt is rejected during admission or replay checks
- **THEN** context MUST report decision event marking rejection and including a reason string
