## ADDED Requirements

### Requirement: Passive pending connections require explicit acceptance
For passive connections, the implementation MUST place new Initial traffic into a pending state, invoke the new-connection decision callback, and complete connection promotion only after `accept()` succeeds and the peer handshake completion signal is received.

#### Scenario: Callback-approved connection is accepted later
- **WHEN** a server receives an Initial packet for an unknown destination CID, the new-connection callback returns true, and the application calls `accept()` successfully
- **THEN** the server MUST keep the connection pending until handshake completion, then promote it to connected

#### Scenario: Immediate inline accept is valid
- **WHEN** the new-connection callback calls `accept()` immediately and `accept()` succeeds
- **THEN** the server MUST allow passive handshake progression without requiring another accept call

#### Scenario: Rejected pending connection is not promoted
- **WHEN** the new-connection callback returns false for a pending passive connection
- **THEN** the implementation MUST reject the attempt and MUST NOT promote that pending connection to connected
