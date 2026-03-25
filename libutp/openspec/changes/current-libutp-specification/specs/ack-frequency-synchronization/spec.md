## ADDED Requirements

### Requirement: AckFrequency synchronizes receiver ACK policy and sender reordering threshold
When a valid AckFrequency frame is received, the implementation MUST update connection ACK generation policy and MUST propagate reordering threshold to sender-side loss detection.

#### Scenario: Reordering threshold update applies on both sides
- **WHEN** an endpoint receives AckFrequency with a lower reordering threshold than current
- **THEN** the receiver ACK policy and sender loss detector MUST both use the new threshold for subsequent decisions

#### Scenario: Gap at threshold triggers immediate ACK
- **WHEN** receive gap meets current reordering threshold
- **THEN** the implementation MUST send ACK immediately instead of waiting for delayed-ACK timer
