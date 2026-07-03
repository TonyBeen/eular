## ADDED Requirements

### Requirement: Public API failures MUST return -1 and expose error via GetLastError
All public-facing API methods that can fail MUST return `-1` on failure and MUST set `GetLastError()` to a concrete `UTP_ERR_*` code.

#### Scenario: Context.accept would block
- **WHEN** application calls `Context::accept()` and there is no pending incoming connection
- **THEN** API MUST return `-1`
- **AND** `GetLastError()` MUST be `UTP_ERR_WOULD_BLOCK`

#### Scenario: Context.connect0Rtt invalid parameters
- **WHEN** application calls `Context::connect0Rtt()` with invalid input
- **THEN** API MUST return `-1`
- **AND** `GetLastError()` MUST be `UTP_ERR_INVALID_PARAM`

### Requirement: Public Stream API MUST keep POSIX read/write success semantics
Public stream data-path methods MUST preserve success return meanings while using POSIX failure semantics.

#### Scenario: Stream.read returns EOF
- **WHEN** peer FIN is received and readable buffer is empty
- **THEN** `Stream::read()` MUST return `0` (EOF)

#### Scenario: Stream.write or commitWrite fails
- **WHEN** `Stream::write()` or `Stream::commitWrite()` cannot proceed due to invalid state or flow-control conditions
- **THEN** method MUST return `-1`
- **AND** `GetLastError()` MUST contain specific failure code

### Requirement: Public Connection.createStream MUST use POSIX failure style
`Connection::createStream()` MUST return stream id on success and MUST return `-1` on failure with `GetLastError()` set.

#### Scenario: Stream creation exceeds stream limit
- **WHEN** local creation quota for a stream type is exhausted
- **THEN** `Connection::createStream()` MUST return `-1`
- **AND** `GetLastError()` MUST be `UTP_ERR_STREAM_LIMIT_ERROR`
