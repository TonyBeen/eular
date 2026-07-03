## ADDED Requirements

### Requirement: Address change enters validating and needs exact challenge-response match
The transport SHALL treat peer address change as unvalidated until challenge-response succeeds. A previously validated path MUST transition to validating on address change. The path validator MUST accept only exact challenge-byte match in PathResponse and ignore mismatches.

#### Scenario: Address change starts validation
- **WHEN** a validated connection observes same peer host on different peer port
- **THEN** path state MUST transition to validating and require validation before normal validated status resumes

#### Scenario: Exact response validates path
- **WHEN** a validating path receives PathResponse containing exact in-flight challenge bytes
- **THEN** path MUST transition to validated and stop requiring validation

#### Scenario: Mismatched response is ignored
- **WHEN** PathResponse challenge bytes do not match in-flight challenge
- **THEN** implementation MUST ignore response and keep path validating

### Requirement: Validation retry budget is bounded
The path validator MUST allow bounded retries and MUST mark path failed when retry attempts are exhausted without matching response.

#### Scenario: Retry exhaustion fails path
- **WHEN** validator reaches retry limit without receiving matching response before each timeout
- **THEN** path MUST transition to failed and MUST stop indicating more retries are allowed
