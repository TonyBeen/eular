## ADDED Requirements

### Requirement: Stream priority MUST influence new data scheduling order
The sender MUST consider per-stream priority when selecting stream data for new transmissions, while preserving correctness of retransmission and control traffic handling.

#### Scenario: Higher-priority stream is preferred for new data
- **WHEN** two writable streams both have pending new data and one has a higher priority
- **THEN** sender MUST prefer the higher-priority stream for new data scheduling under the same send budget

#### Scenario: Flow-control blocked high-priority stream is skipped
- **WHEN** a higher-priority stream has pending data but cannot send because stream-level or connection-level flow control blocks it
- **THEN** sender MUST skip that stream and select the highest-priority stream that remains sendable under flow-control and send-budget constraints

### Requirement: Priority scheduling MUST include starvation protection
The scheduler MUST provide bounded service for lower-priority streams under sustained higher-priority load.

#### Scenario: Lower-priority stream still receives service
- **WHEN** higher-priority streams remain continuously backlogged
- **THEN** lower-priority stream MUST still be granted bounded send opportunities according to starvation-protection policy

### Requirement: DRR policy MUST be supported as a first-release scheduling policy
The implementation MUST support DRR scheduling in addition to strict-priority scheduling for new-data stream selection.

#### Scenario: DRR policy provides bounded fair service among eligible streams
- **WHEN** runtime policy is set to DRR and multiple sendable streams are backlogged
- **THEN** sender MUST allocate new-data send opportunities according to DRR deficit/quantum rules instead of strict-priority-only selection

### Requirement: Priority MUST NOT violate reliability and control-path correctness
Priority scheduling MUST NOT bypass retransmission precedence or mandatory control-frame processing.

#### Scenario: Lost data retransmission still precedes new low-priority data
- **WHEN** retransmission queue is non-empty and a low-priority stream has new data
- **THEN** sender MUST process retransmission before scheduling new low-priority stream data

### Requirement: Scheduler policy hot switch MUST preserve correctness boundaries
The implementation MUST support runtime switching among disabled scheduling, strict-priority scheduling, and DRR scheduling for future new-data selection without reordering already built packets or retransmission state.

#### Scenario: Enabling scheduler applies to subsequent new data only
- **WHEN** runtime policy changes from disabled scheduling to strict-priority scheduling or DRR scheduling
- **THEN** implementation MUST rebuild scheduler runtime state for future new-data selection and MUST NOT reorder already built packets or retransmission queues

#### Scenario: Disabling scheduler stops priority-based new-data selection
- **WHEN** runtime policy changes from strict-priority scheduling or DRR scheduling to disabled scheduling
- **THEN** implementation MUST stop priority-based new-data selection for future packets while preserving correctness of already tracked retransmission and control state

#### Scenario: Runtime switch between strict-priority and DRR resets policy-local runtime state only
- **WHEN** runtime policy changes between strict-priority scheduling and DRR scheduling
- **THEN** implementation MUST reset policy-local runtime state (such as aging window or DRR deficit) for future new-data selection while preserving retransmission and control correctness
