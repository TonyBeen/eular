## ADDED Requirements

### Requirement: Stream identifier assignment and admission follow role and per-type limits
The transport MUST assign locally created stream identifiers according to endpoint role and stream type. It MUST enforce per-type creation quotas only against locally initiated active streams. Peer-initiated streams MUST be admitted only when role bits and stream ordinal are valid for the receiver.

#### Scenario: Server endpoint emits server-initiated IDs
- **WHEN** a server-side endpoint creates one bidirectional stream and one unidirectional stream
- **THEN** each stream identifier MUST follow the server-initiated pattern for its type

#### Scenario: Peer-created streams do not consume local creation quota
- **WHEN** a connection already contains peer-initiated streams and the application queries remaining creatable stream counts
- **THEN** remaining quota MUST be computed from locally initiated active streams only

#### Scenario: Invalid peer stream ID is rejected
- **WHEN** a stream frame arrives with an identifier that violates receiver role expectations or exceeds advertised per-type limit
- **THEN** the implementation MUST reject ingress for that stream with a stream-state or stream-limit error
