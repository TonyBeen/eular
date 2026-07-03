## ADDED Requirements

### Requirement: Loss detection retransmits payload as new send attempts
The recovery layer SHALL detect lost packets from ACK feedback and retransmission timers, and MUST retransmit recoverable payload through new transmission attempts instead of keeping the original attempt in flight.

#### Scenario: Marked loss schedules retransmission
- **WHEN** ACK or timeout processing marks a sent packet as lost while payload remains recoverable
- **THEN** the implementation MUST move that payload into retransmission handling and send a replacement attempt
