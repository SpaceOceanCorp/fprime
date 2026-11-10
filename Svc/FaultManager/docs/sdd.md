# Svc::FaultManager

`Svc::FaultManager` turns fault announcements from other components into system responses. It detects nothing itself. For each fault in a table loaded from a file, it applies a ground-tunable filter (state, persistence, latch, event limit). When a fault is declared, it runs that fault's response sequence through a sequencer. Responses use commands that already exist, so components need no fault-handling ports beyond the announcement.

It follows the announce / monitor / respond / complete model of discussion nasa/fprime#2536, with responses as ground-editable sequences. Persistence counting, the PASSIVE state and per-fault event limits follow the actionpoints of the cFS Limit Checker (LC).

It complements `Svc::Health` (liveness of active components) and `Svc::FatalHandler` (the terminal path after a FATAL event).

## 1. Requirements

| Requirement | Description | Rationale | Verification |
|---|---|---|---|
| SVC-FAULTMANAGER-001 | `Svc::FaultManager` shall accept fault announcements, each carrying a fault identifier and one 32-bit data word, from multiple components. | Detection stays with the components that understand their data. | Unit Test |
| SVC-FAULTMANAGER-002 | `Svc::FaultManager` shall accept fault clears, each carrying a fault identifier, that reset the fault's consecutive count. | Lets a detector report that a condition has gone away, so persistence counts consecutive announcements. | Unit Test |
| SVC-FAULTMANAGER-003 | `Svc::FaultManager` shall load a fault table from a file, giving for each fault its state, persistence count, latch and clear-on-success flags, response priority, event limit and response sequence. | Fault behavior is tunable without rebuilding the software. | Unit Test |
| SVC-FAULTMANAGER-004 | `Svc::FaultManager` shall support a fault state of ACTIVE, PASSIVE or DISABLED. | PASSIVE declares without responding, for commissioning and testing. | Unit Test |
| SVC-FAULTMANAGER-005 | `Svc::FaultManager` shall declare a fault in the ACTIVE or PASSIVE state when it is announced its persistence count of consecutive times. | Persistence keeps transient symptoms from triggering a response. | Unit Test |
| SVC-FAULTMANAGER-006 | `Svc::FaultManager` shall start the response sequence of a declared ACTIVE fault unless responses are inhibited. | Responses reuse existing commands through sequences. | Unit Test |
| SVC-FAULTMANAGER-007 | `Svc::FaultManager` shall run one response at a time. A declared fault with a higher priority than the running response shall cancel it and start its own; otherwise its response shall start when the running one completes. | The more important response wins. | Unit Test |
| SVC-FAULTMANAGER-008 | `Svc::FaultManager` shall latch a declared fault whose entry sets the latch flag, and shall count, but not declare, a latched fault's announcements. | Prevents repeated responses to one ongoing fault. | Unit Test |
| SVC-FAULTMANAGER-009 | When a response completes successfully, `Svc::FaultManager` shall clear the latch and consecutive count of a fault whose entry sets the clear-on-success flag. When a response fails, it shall emit a warning and keep the latch. | A failed response needs ground attention. | Unit Test |
| SVC-FAULTMANAGER-010 | `Svc::FaultManager` shall limit the declaration events of each fault to its event limit until the fault is reset. | A repeating fault must not flood the event log. | Unit Test |
| SVC-FAULTMANAGER-011 | `Svc::FaultManager` shall emit an event for an announcement, clear or command naming a fault that is not in the table, and shall not otherwise act on it. | Misconfiguration is visible without causing a response. | Unit Test |
| SVC-FAULTMANAGER-012 | `Svc::FaultManager` shall provide commands to set a fault's state, reset a fault, reload the table, and inhibit or allow all responses. | Ground control of fault protection. | Unit Test |
| SVC-FAULTMANAGER-013 | `Svc::FaultManager` shall report in telemetry the response in progress, the last declared fault, the number of declarations, whether responses are inhibited, and each fault's state, consecutive count, total count and latch. | Ground visibility. | Unit Test |
| SVC-FAULTMANAGER-014 | `Svc::FaultManager` shall reject a table that fails validation, or a reload while a response is running, and keep the table in use. | A bad upload must not disable fault protection. | Unit Test |
| SVC-FAULTMANAGER-015 | `Svc::FaultManager` shall respond to health pings. | Standard for active components. | Unit Test |

## 2. Ports

| Kind | Name | Type | Description |
|---|---|---|---|
| async input | `faultIn` | `[FaultManagerAnnouncePorts] Svc.FaultAnnounce` | Fault announcements (fault ID, data) |
| async input | `faultClearIn` | `[FaultManagerAnnouncePorts] Svc.FaultClear` | Fault clears (fault ID) |
| output | `seqRunOut` | `Svc.CmdSeqIn` | Starts a response sequence (to `Svc::SeqDispatcher` or a sequencer) |
| output | `seqCancelOut` | `Svc.CmdSeqCancel` | Cancels the running response |
| async input | `seqDoneIn` | `Fw.CmdResponse` | Completion status of the response sequence |
| async input | `pingIn` | `Svc.Ping` | Health ping |
| output | `pingOut` | `Svc.Ping` | Health ping response |

Plus the standard command, event, telemetry and time ports.

## 3. Configuration

- `FaultManagerCfg.MaxFaults` (`default/config/FaultManagerCfg.fpp`, default 32): table capacity.
- `FaultManagerAnnouncePorts` (`default/config/AcConstants.fpp`, default 8): number of announcement and clear ports.
- `configure(tableFileName)`: loads the initial table. Call it after the ports are connected.

## 4. Fault table file

All integers are big-endian.

| Field | Size | Notes |
|---|---|---|
| magic | U32 | `0x464D5431` ("FMT1") |
| count | U16 | 0 to `MaxFaults` |
| entries | count × variable | see below |
| crc | U32 | CRC32 (`Utils::Hash`) of every byte before it |

Each entry:

| Field | Size | Notes |
|---|---|---|
| faultId | U32 | Unique within the table |
| state | U8 | 0 ACTIVE, 1 PASSIVE, 2 DISABLED |
| persistence | U8 | 1 to 255 consecutive announcements |
| flags | U8 | bit 0: latch; bit 1: clear on success; other bits must be 0 |
| priority | U8 | Higher wins |
| eventLimit | U8 | Maximum declaration events until reset; 0 = no limit |
| nameLength | U8 | 1 to `FileNameStringSize - 1` |
| sequence | nameLength bytes | Response sequence file name, no NUL |

A table is rejected (`TableRejected`, with reason) if it cannot be read, has the wrong size or magic, has more than `MaxFaults` entries, fails the CRC, has an invalid entry, or repeats a fault ID.

## 5. Behavior

On an announcement for a fault that is not DISABLED, the total count is incremented. If the fault is not latched, its consecutive count is incremented, and when the count reaches the persistence value the fault is declared and the count goes back to 0.

On declaration, the fault latches if its latch flag is set, and `FaultDeclared` is emitted (subject to the event limit). If the fault is ACTIVE and responses are not inhibited, its response is requested:

- With no response running, it starts.
- If a lower-priority response is running, it is canceled (`ResponsePreempted`). The new response starts when the canceled sequence reports done.
- Otherwise it waits (`ResponseQueued`) and starts after the running response, in priority order.

On `seqDoneIn`, a successful response emits `ResponseCompleted` and, with the clear-on-success flag, clears the latch. A failed response emits `ResponseFailed` and keeps the latch. The next queued response then starts.

A declaration made while responses are inhibited emits `ResponseInhibited` and is not run later.

## 6. Commands

| Name | Description |
|---|---|
| `FAULT_SET_STATE` | Set a fault's state. Leaving ACTIVE drops a queued response. |
| `FAULT_RESET` | Clear a fault's counts, latch and event limit |
| `TABLE_RELOAD` | Load a new table; refused while a response is running |
| `RESPONSES_INHIBIT` | Stop starting responses |
| `RESPONSES_ALLOW` | Start responses again |

## 7. Events and telemetry

Events: `FaultDeclared`, `ResponseStarted`, `ResponseCompleted`, `ResponseFailed`, `ResponseQueued`, `ResponsePreempted`, `ResponseInhibited`, `UnknownFault` (throttled), `EventLimitReached`, `TableLoaded`, `TableRejected`, `TableReloadBusy`, `FaultStateSet`, `FaultReset`, `ResponsesInhibitSet`, `UnexpectedSequenceDone`.

Telemetry: `ActiveResponseFault`, `LastDeclaredFault`, `DeclarationCount`, `ResponsesInhibited`, and `FaultTable` (state, consecutive count, total count and latch of every entry).

## 8. Unit tests

| Test | Requirements |
|---|---|
| `testTableLoad`, `testTableRejects`, `testReloadBusy` | 003, 014 |
| `testPersistenceAndResponse` | 001, 005, 006 |
| `testClearResetsPersistence` | 002 |
| `testPassiveAndDisabled`, `testSetStateCommand` | 004, 012 |
| `testLatchAndReset`, `testClearOnSuccess`, `testResponseFailureKeepsLatch` | 008, 009, 012 |
| `testPriorityPreemption`, `testLowerPriorityQueued` | 007 |
| `testInhibit` | 006, 012 |
| `testEventLimit` | 010 |
| `testUnknownFault` | 011 |
| `testPing` | 015 |
| telemetry checked throughout | 013 |
