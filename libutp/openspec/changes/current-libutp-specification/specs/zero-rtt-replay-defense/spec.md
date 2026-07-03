## ADDED Requirements

### Requirement: 0-RTT replay defense rejects duplicate nonce tuples within replay window
The implementation MUST maintain replay window state keyed by token-associated connection identity and packet number. Duplicate tuple observations inside replay window MUST be rejected. Tuples MAY be admitted again after replay entry expires and is purged.

#### Scenario: Duplicate tuple in window is rejected
- **WHEN** the same ticket-associated identity and packet number is observed again before replay window expiry
- **THEN** second observation MUST be rejected as replayed 0-RTT traffic

#### Scenario: Purged tuple may be re-admitted
- **WHEN** previously recorded tuple has aged out and replay cache is purged
- **THEN** same tuple MAY be recorded again as fresh admission candidate
