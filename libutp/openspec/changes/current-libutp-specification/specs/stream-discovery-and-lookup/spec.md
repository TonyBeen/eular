## ADDED Requirements

### Requirement: Stream lookup and creation expose stable retrieval semantics
An established connection SHALL allow stream creation according to limits and SHALL support stream lookup by stream identifier. `getStream(streamId)` MUST return the stream object for an existing stream and MUST return no stream for an unknown identifier.

#### Scenario: Created stream can be queried
- **WHEN** an application creates a stream on a connected connection and queries that stream identifier
- **THEN** the implementation MUST return a non-null stream object with the same identifier

#### Scenario: Unknown stream lookup returns empty
- **WHEN** an application queries a stream identifier that does not exist on the connection
- **THEN** the implementation MUST return no stream
