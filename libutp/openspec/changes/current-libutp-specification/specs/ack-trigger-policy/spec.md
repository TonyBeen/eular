## ADDED Requirements

### Requirement: ACK generation is bounded by packet-threshold and timer triggers
The transport SHALL generate acknowledgements for ack-eliciting traffic using packet-threshold and time-threshold triggers. If follow-up packets do not satisfy packet threshold, delayed ACK MUST be sent when ACK delay expires.

#### Scenario: Delayed ACK timer flushes a lone packet
- **WHEN** an endpoint receives an ack-eliciting packet and no follow-up packets arrive before ACK delay expires
- **THEN** the implementation MUST emit ACK for that packet and clear pending ack-eliciting count
