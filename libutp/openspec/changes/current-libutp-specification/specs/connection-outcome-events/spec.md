## ADDED Requirements

### Requirement: Connection result events distinguish success and failure
The public context callback surface MUST report outcomes through distinct events. Successful attempts MUST invoke the connected callback with a connection handle. Failed attempts MUST invoke the connect-error callback with a connect-attempt descriptor that identifies attempt type.

#### Scenario: Successful connect reports connection handle
- **WHEN** a connect attempt completes handshake successfully
- **THEN** the implementation MUST invoke the connected callback with a non-null connection handle

#### Scenario: Failed connect reports attempt metadata
- **WHEN** a connect attempt fails during validation or handshake processing
- **THEN** the implementation MUST invoke the connect-error callback with the attempt type and failure-path metadata
