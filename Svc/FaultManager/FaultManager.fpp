module Svc {

  @ Report that a fault was detected
  port FaultAnnounce(
                      faultId: U32 @< Project-defined fault identifier
                      data: U32 @< Fault-specific data, reported in the declaration event
                    )

  @ Report that a previously announced fault condition is no longer present
  port FaultClear(
                   faultId: U32 @< Project-defined fault identifier
                 )

  @ Fault response state of one fault table entry
  enum FaultState : U8 {
    ACTIVE = 0 @< Counts announcements, declares the fault and runs its response
    PASSIVE = 1 @< Counts announcements and declares the fault, but never runs a response
    DISABLED = 2 @< Ignores announcements
  }

  @ Why a fault table was rejected
  enum FaultTableError : U8 {
    OPEN_FAILED = 0 @< The file could not be opened or read
    BAD_SIZE = 1 @< The file size does not match its entry count
    BAD_MAGIC = 2 @< The file does not start with the table magic number
    TOO_MANY_ENTRIES = 3 @< The entry count exceeds FaultManagerCfg.MaxFaults
    BAD_CRC = 4 @< The CRC32 does not match the file contents
    BAD_ENTRY = 5 @< An entry has an invalid state, persistence or sequence name
    DUPLICATE_ID = 6 @< Two entries share a fault identifier
  }

  @ Status of one fault, for telemetry
  struct FaultStatus {
    faultId: U32 @< Fault identifier
    $state: FaultState @< Current state
    consecutive: U8 @< Consecutive announcements since the last declaration, clear or reset
    total: U16 @< Announcements received since the last reset (saturates)
    latched: bool @< Latched after a declaration
  }

  @ Status of every fault table entry
  array FaultStatusTable = [FaultManagerCfg.MaxFaults] FaultStatus

  @ Filters fault announcements and runs response sequences
  active component FaultManager {

    # ----------------------------------------------------------------------
    # Fault ports
    # ----------------------------------------------------------------------

    @ Fault announcements from detecting components
    async input port faultIn: [FaultManagerAnnouncePorts] FaultAnnounce

    @ Fault clears from detecting components
    async input port faultClearIn: [FaultManagerAnnouncePorts] FaultClear

    # ----------------------------------------------------------------------
    # Response ports
    # ----------------------------------------------------------------------

    @ Runs a response sequence
    output port seqRunOut: Svc.CmdSeqIn

    @ Cancels the running response sequence
    output port seqCancelOut: Svc.CmdSeqCancel

    @ Completion status of the response sequence
    async input port seqDoneIn: Fw.CmdResponse

    # ----------------------------------------------------------------------
    # Health
    # ----------------------------------------------------------------------

    @ Ping input port
    async input port pingIn: Svc.Ping

    @ Ping output port
    output port pingOut: Svc.Ping

    # ----------------------------------------------------------------------
    # Commands
    # ----------------------------------------------------------------------

    @ Set the state of a fault
    async command FAULT_SET_STATE(
                                   faultId: U32 @< Fault identifier
                                   $state: FaultState @< New state
                                 ) \
      opcode 0

    @ Clear the counts, latch and event limit of a fault
    async command FAULT_RESET(
                               faultId: U32 @< Fault identifier
                             ) \
      opcode 1

    @ Load a new fault table; refused while a response is running
    async command TABLE_RELOAD(
                                fileName: string size FileNameStringSize @< Fault table file
                              ) \
      opcode 2

    @ Stop starting responses (declarations are still counted and reported)
    async command RESPONSES_INHIBIT() \
      opcode 3

    @ Allow responses again
    async command RESPONSES_ALLOW() \
      opcode 4

    # ----------------------------------------------------------------------
    # Events
    # ----------------------------------------------------------------------

    @ A fault was declared
    event FaultDeclared(
                         faultId: U32 @< Fault identifier
                         data: U32 @< Data from the announcement
                         $state: FaultState @< State of the fault
                       ) \
      severity warning high \
      id 0 \
      format "Fault {} declared (data {}, state {})"

    @ A response sequence was started
    event ResponseStarted(
                           faultId: U32 @< Fault identifier
                           sequence: string size FileNameStringSize @< Response sequence
                         ) \
      severity activity high \
      id 1 \
      format "Fault {} response {} started"

    @ A response sequence completed successfully
    event ResponseCompleted(
                             faultId: U32 @< Fault identifier
                           ) \
      severity activity high \
      id 2 \
      format "Fault {} response completed"

    @ A response sequence failed
    event ResponseFailed(
                          faultId: U32 @< Fault identifier
                          response: Fw.CmdResponse @< Sequence completion status
                        ) \
      severity warning high \
      id 3 \
      format "Fault {} response failed: {}"

    @ A response is waiting behind a response of equal or higher priority
    event ResponseQueued(
                          faultId: U32 @< Fault identifier
                        ) \
      severity activity low \
      id 4 \
      format "Fault {} response queued"

    @ A running response was canceled for a higher-priority one
    event ResponsePreempted(
                             faultId: U32 @< Fault whose response was canceled
                             byFaultId: U32 @< Fault whose response replaces it
                           ) \
      severity warning low \
      id 5 \
      format "Fault {} response canceled for fault {}"

    @ A declared fault did not start a response because responses are inhibited
    event ResponseInhibited(
                             faultId: U32 @< Fault identifier
                           ) \
      severity warning low \
      id 6 \
      format "Fault {} response not started: responses inhibited"

    @ An announcement or clear named a fault that is not in the table
    event UnknownFault(
                        faultId: U32 @< Fault identifier
                      ) \
      severity warning low \
      id 7 \
      format "Fault {} is not in the fault table" \
      throttle 10

    @ A fault reached its event limit; later declarations are counted but not reported until a reset
    event EventLimitReached(
                             faultId: U32 @< Fault identifier
                           ) \
      severity warning low \
      id 8 \
      format "Fault {} reached its event limit"

    @ A fault table was loaded
    event TableLoaded(
                       fileName: string size FileNameStringSize @< Fault table file
                       count: U16 @< Number of entries
                     ) \
      severity activity high \
      id 9 \
      format "Fault table {} loaded with {} entries"

    @ A fault table was rejected; the table in use is unchanged
    event TableRejected(
                         fileName: string size FileNameStringSize @< Fault table file
                         reason: FaultTableError @< Why it was rejected
                       ) \
      severity warning high \
      id 10 \
      format "Fault table {} rejected: {}"

    @ A table reload was refused because a response is running
    event TableReloadBusy() \
      severity warning low \
      id 11 \
      format "Fault table reload refused: a response is running"

    @ A fault state was set by command
    event FaultStateSet(
                         faultId: U32 @< Fault identifier
                         $state: FaultState @< New state
                       ) \
      severity activity high \
      id 12 \
      format "Fault {} state set to {}"

    @ A fault was reset by command
    event FaultReset(
                      faultId: U32 @< Fault identifier
                    ) \
      severity activity high \
      id 13 \
      format "Fault {} reset"

    @ Responses were inhibited or allowed by command
    event ResponsesInhibitSet(
                               inhibited: bool @< True if responses are now inhibited
                             ) \
      severity activity high \
      id 14 \
      format "Fault responses inhibited: {}"

    @ A sequence completion arrived with no response running
    event UnexpectedSequenceDone(
                                  response: Fw.CmdResponse @< Reported status
                                ) \
      severity warning low \
      id 15 \
      format "Sequence completion ({}) received with no fault response running"

    # ----------------------------------------------------------------------
    # Telemetry
    # ----------------------------------------------------------------------

    @ Fault whose response is running (0 if none)
    telemetry ActiveResponseFault: U32 id 0

    @ Most recently declared fault
    telemetry LastDeclaredFault: U32 id 1

    @ Number of declarations since startup
    telemetry DeclarationCount: U32 id 2

    @ True while responses are inhibited
    telemetry ResponsesInhibited: bool id 3

    @ Status of every fault table entry
    telemetry FaultTable: FaultStatusTable id 4

    # ----------------------------------------------------------------------
    # Standard ports
    # ----------------------------------------------------------------------

    @ Port for requesting the current time
    time get port timeCaller

    @ Port for sending command registrations
    command reg port cmdRegOut

    @ Port for receiving commands
    command recv port cmdIn

    @ Port for sending command responses
    command resp port cmdResponseOut

    @ Port for sending textual representation of events
    text event port logTextOut

    @ Port for sending events to downlink
    event port logOut

    @ Port for sending telemetry channels to downlink
    telemetry port tlmOut

  }

}
