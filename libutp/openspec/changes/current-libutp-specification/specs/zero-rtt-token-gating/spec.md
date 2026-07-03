## ADDED Requirements

### Requirement: 0-RTT token admission is peer-bound and lifetime-bound
The 0-RTT admission path SHALL validate token peer binding, token lifetime, and encoded resumption metadata before accepting early data. Peer mismatch or expiry MUST be rejected.

#### Scenario: Valid token is admitted with metadata
- **WHEN** an endpoint validates a well-formed, unexpired token bound to packet peer address
- **THEN** implementation MUST accept token, recover ticket CID and encryption mode metadata, and continue 0-RTT admission

#### Scenario: Peer mismatch rejects token
- **WHEN** presented token peer binding differs from packet peer address
- **THEN** implementation MUST reject token and MUST NOT accept early data

#### Scenario: Expired token rejects admission
- **WHEN** token age exceeds configured maximum lifetime
- **THEN** implementation MUST reject token and MUST NOT treat resumption metadata as valid
