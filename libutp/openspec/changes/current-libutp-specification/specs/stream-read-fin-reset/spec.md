## ADDED Requirements

### Requirement: Stream data readability, FIN, and reset are observable
The stream layer SHALL expose readable state for in-order data, SHALL preserve readable data until consumed, and MUST represent peer FIN and reset events in stream state and callbacks.

#### Scenario: In-order data becomes readable and consumable
- **WHEN** a stream receives an in-order data frame at the current receive offset
- **THEN** the stream MUST become readable, allow buffered data consumption, and clear readable state after full consumption

#### Scenario: Out-of-order data remains unreadable before gap closure
- **WHEN** a stream receives data with a gap ahead of current receive offset
- **THEN** the implementation MUST NOT expose that data as readable until missing earlier offset is supplied

#### Scenario: Peer FIN maps to receive end-of-stream
- **WHEN** a stream receives FIN at current receive offset and no unread payload remains
- **THEN** the stream MUST transition receive side to end-of-stream and an empty read MUST report EOF

#### Scenario: Reset notifies application with error code
- **WHEN** a stream receives reset carrying an error code
- **THEN** the stream MUST record reset, close stream state, and invoke reset callback with that error code
