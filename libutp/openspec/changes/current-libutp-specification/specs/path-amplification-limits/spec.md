## ADDED Requirements

### Requirement: Validating paths enforce amplification limits for ordinary payload
While a path is validating, the implementation MUST restrict normal application data transmission according to amplification policy, while allowing validation-critical control traffic.

#### Scenario: Validating path gates ordinary stream payload
- **WHEN** a path is validating and application tries to send ordinary stream traffic beyond allowed amplification budget
- **THEN** implementation MUST block or defer ordinary traffic until path validates or budget permits transmission
