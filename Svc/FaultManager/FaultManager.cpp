// ======================================================================
// \title  FaultManager.cpp
// \brief  cpp file for FaultManager component implementation class
// ======================================================================

#include "Svc/FaultManager/FaultManager.hpp"
#include "Fw/Types/Assert.hpp"
#include "Os/File.hpp"
#include "Utils/Hash/Hash.hpp"

namespace Svc {

namespace {
U32 readU32(const U8* p) {
    return (static_cast<U32>(p[0]) << 24) | (static_cast<U32>(p[1]) << 16) | (static_cast<U32>(p[2]) << 8) |
           static_cast<U32>(p[3]);
}
U16 readU16(const U8* p) {
    return static_cast<U16>((static_cast<U16>(p[0]) << 8) | static_cast<U16>(p[1]));
}
}  // namespace

// ----------------------------------------------------------------------
// Component construction and destruction
// ----------------------------------------------------------------------

FaultManager ::FaultManager(const char* const compName)
    : FaultManagerComponentBase(compName),
      m_entries(),
      m_count(0),
      m_active(-1),
      m_cancelPending(false),
      m_inhibited(false),
      m_declarations(0),
      m_lastDeclared(0),
      m_fileBuffer(),
      m_staging() {}

FaultManager ::~FaultManager() {}

bool FaultManager ::configure(const char* tableFileName) {
    FW_ASSERT(tableFileName != nullptr);
    const bool loaded = this->loadTable(Fw::String(tableFileName));
    this->writeTelemetry();
    return loaded;
}

// ----------------------------------------------------------------------
// Handler implementations for typed input ports
// ----------------------------------------------------------------------

void FaultManager ::faultIn_handler(FwIndexType portNum, U32 faultId, U32 data) {
    (void)portNum;
    const I32 idx = this->findEntry(faultId);
    if (idx < 0) {
        this->log_WARNING_LO_UnknownFault(faultId);
        return;
    }
    Entry& entry = this->m_entries[idx];
    if (entry.state == FaultState::DISABLED) {
        return;
    }
    if (entry.total < 0xFFFF) {
        entry.total++;
    }
    if (!entry.latched) {
        if (entry.consecutive < 0xFF) {
            entry.consecutive++;
        }
        if (entry.consecutive >= entry.persistence) {
            entry.consecutive = 0;
            this->declare(idx, data);
        }
    }
    this->writeTelemetry();
}

void FaultManager ::faultClearIn_handler(FwIndexType portNum, U32 faultId) {
    (void)portNum;
    const I32 idx = this->findEntry(faultId);
    if (idx < 0) {
        this->log_WARNING_LO_UnknownFault(faultId);
        return;
    }
    this->m_entries[idx].consecutive = 0;
    this->writeTelemetry();
}

void FaultManager ::seqDoneIn_handler(FwIndexType portNum,
                                      FwOpcodeType opCode,
                                      U32 cmdSeq,
                                      const Fw::CmdResponse& response) {
    (void)portNum;
    (void)opCode;
    (void)cmdSeq;
    if (this->m_active < 0) {
        this->log_WARNING_LO_UnexpectedSequenceDone(response);
        return;
    }
    Entry& entry = this->m_entries[this->m_active];
    if (this->m_cancelPending) {
        // The preemption was reported when the cancel was sent
        this->m_cancelPending = false;
    } else if (response == Fw::CmdResponse::OK) {
        this->log_ACTIVITY_HI_ResponseCompleted(entry.faultId);
        if ((entry.flags & FLAG_CLEAR_ON_SUCCESS) != 0) {
            entry.latched = false;
            entry.consecutive = 0;
        }
    } else {
        this->log_WARNING_HI_ResponseFailed(entry.faultId, response);
    }
    this->m_active = -1;
    this->startNextResponse();
    this->writeTelemetry();
}

void FaultManager ::pingIn_handler(FwIndexType portNum, U32 key) {
    (void)portNum;
    this->pingOut_out(0, key);
}

// ----------------------------------------------------------------------
// Handler implementations for commands
// ----------------------------------------------------------------------

void FaultManager ::FAULT_SET_STATE_cmdHandler(FwOpcodeType opCode,
                                               U32 cmdSeq,
                                               U32 faultId,
                                               const Svc::FaultState& state) {
    const I32 idx = this->findEntry(faultId);
    if (idx < 0) {
        this->log_WARNING_LO_UnknownFault(faultId);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    Entry& entry = this->m_entries[idx];
    entry.state = state;
    if (state != FaultState::ACTIVE) {
        entry.queued = false;
    }
    this->log_ACTIVITY_HI_FaultStateSet(faultId, state);
    this->writeTelemetry();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void FaultManager ::FAULT_RESET_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 faultId) {
    const I32 idx = this->findEntry(faultId);
    if (idx < 0) {
        this->log_WARNING_LO_UnknownFault(faultId);
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::VALIDATION_ERROR);
        return;
    }
    resetRuntime(this->m_entries[idx]);
    this->log_ACTIVITY_HI_FaultReset(faultId);
    this->writeTelemetry();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void FaultManager ::TABLE_RELOAD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& fileName) {
    if (this->m_active >= 0) {
        this->log_WARNING_LO_TableReloadBusy();
        this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    const bool loaded = this->loadTable(fileName);
    this->writeTelemetry();
    this->cmdResponse_out(opCode, cmdSeq, loaded ? Fw::CmdResponse::OK : Fw::CmdResponse::EXECUTION_ERROR);
}

void FaultManager ::RESPONSES_INHIBIT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    this->m_inhibited = true;
    this->log_ACTIVITY_HI_ResponsesInhibitSet(true);
    this->writeTelemetry();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

void FaultManager ::RESPONSES_ALLOW_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) {
    this->m_inhibited = false;
    this->log_ACTIVITY_HI_ResponsesInhibitSet(false);
    this->startNextResponse();
    this->writeTelemetry();
    this->cmdResponse_out(opCode, cmdSeq, Fw::CmdResponse::OK);
}

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

bool FaultManager ::loadTable(const Fw::StringBase& fileName) {
    Fw::LogStringArg logName(fileName);
    Os::File file;
    if (file.open(fileName.toChar(), Os::File::OPEN_READ) != Os::File::OP_OK) {
        this->log_WARNING_HI_TableRejected(logName, FaultTableError::OPEN_FAILED);
        return false;
    }
    FwSizeType size = sizeof(this->m_fileBuffer);
    const Os::File::Status status = file.read(this->m_fileBuffer, size, Os::File::WAIT);
    file.close();
    if (status != Os::File::OP_OK) {
        this->log_WARNING_HI_TableRejected(logName, FaultTableError::OPEN_FAILED);
        return false;
    }
    U16 count = 0;
    FaultTableError error = FaultTableError::BAD_SIZE;
    if (!this->parseTable(this->m_fileBuffer, size, this->m_staging, count, error)) {
        this->log_WARNING_HI_TableRejected(logName, error);
        return false;
    }
    for (U16 i = 0; i < count; i++) {
        this->m_entries[i] = this->m_staging[i];
    }
    this->m_count = count;
    this->m_active = -1;
    this->m_cancelPending = false;
    this->log_ACTIVITY_HI_TableLoaded(logName, count);
    return true;
}

bool FaultManager ::parseTable(const U8* data,
                               FwSizeType size,
                               Entry* entries,
                               U16& count,
                               FaultTableError& error) const {
    FW_ASSERT(data != nullptr);
    FW_ASSERT(entries != nullptr);
    if (size < HEADER_SIZE + CRC_SIZE || size > MAX_TABLE_SIZE) {
        error = FaultTableError::BAD_SIZE;
        return false;
    }
    if (readU32(data) != TABLE_MAGIC) {
        error = FaultTableError::BAD_MAGIC;
        return false;
    }
    U32 computed = 0;
    Utils::Hash hash;
    hash.init();
    hash.update(data, size - CRC_SIZE);
    hash.finalize(computed);
    if (computed != readU32(data + size - CRC_SIZE)) {
        error = FaultTableError::BAD_CRC;
        return false;
    }
    const U16 n = readU16(data + 4);
    if (n > FaultManagerCfg::MaxFaults) {
        error = FaultTableError::TOO_MANY_ENTRIES;
        return false;
    }
    FwSizeType pos = HEADER_SIZE;
    const FwSizeType end = size - CRC_SIZE;
    for (U16 i = 0; i < n; i++) {
        if (pos + ENTRY_FIXED_SIZE > end) {
            error = FaultTableError::BAD_SIZE;
            return false;
        }
        Entry& entry = entries[i];
        entry.faultId = readU32(data + pos);
        const U8 state = data[pos + 4];
        entry.persistence = data[pos + 5];
        entry.flags = data[pos + 6];
        entry.priority = data[pos + 7];
        entry.eventLimit = data[pos + 8];
        const U8 nameLength = data[pos + 9];
        pos += ENTRY_FIXED_SIZE;
        if (state > FaultState::DISABLED || entry.persistence == 0 || nameLength == 0 ||
            nameLength >= FileNameStringSize || (entry.flags & ~(FLAG_LATCH | FLAG_CLEAR_ON_SUCCESS)) != 0) {
            error = FaultTableError::BAD_ENTRY;
            return false;
        }
        if (pos + nameLength > end) {
            error = FaultTableError::BAD_SIZE;
            return false;
        }
        char name[FileNameStringSize];
        for (U8 c = 0; c < nameLength; c++) {
            name[c] = static_cast<char>(data[pos + c]);
            if (name[c] == '\0') {
                error = FaultTableError::BAD_ENTRY;
                return false;
            }
        }
        name[nameLength] = '\0';
        pos += nameLength;
        entry.state = static_cast<FaultState::T>(state);
        entry.sequence = name;
        resetRuntime(entry);
        for (U16 j = 0; j < i; j++) {
            if (entries[j].faultId == entry.faultId) {
                error = FaultTableError::DUPLICATE_ID;
                return false;
            }
        }
    }
    if (pos != end) {
        error = FaultTableError::BAD_SIZE;
        return false;
    }
    count = n;
    return true;
}

I32 FaultManager ::findEntry(U32 faultId) const {
    for (U16 i = 0; i < this->m_count; i++) {
        if (this->m_entries[i].faultId == faultId) {
            return static_cast<I32>(i);
        }
    }
    return -1;
}

void FaultManager ::declare(I32 idx, U32 data) {
    FW_ASSERT(idx >= 0 && idx < static_cast<I32>(this->m_count), static_cast<FwAssertArgType>(idx));
    Entry& entry = this->m_entries[idx];
    if ((entry.flags & FLAG_LATCH) != 0) {
        entry.latched = true;
    }
    this->m_declarations++;
    this->m_lastDeclared = entry.faultId;
    if (entry.eventLimit == 0 || entry.eventsSent < entry.eventLimit) {
        entry.eventsSent++;
        this->log_WARNING_HI_FaultDeclared(entry.faultId, data, entry.state);
    } else if (!entry.limitReported) {
        entry.limitReported = true;
        this->log_WARNING_LO_EventLimitReached(entry.faultId);
    }
    if (entry.state != FaultState::ACTIVE) {
        return;
    }
    if (this->m_inhibited) {
        this->log_WARNING_LO_ResponseInhibited(entry.faultId);
        return;
    }
    this->requestResponse(idx);
}

void FaultManager ::requestResponse(I32 idx) {
    Entry& entry = this->m_entries[idx];
    if (this->m_active == idx) {
        // Its response is already running
        return;
    }
    entry.queued = true;
    if (this->m_active < 0) {
        this->startNextResponse();
        return;
    }
    Entry& running = this->m_entries[this->m_active];
    if (!this->m_cancelPending && entry.priority > running.priority) {
        this->m_cancelPending = true;
        this->log_WARNING_LO_ResponsePreempted(running.faultId, entry.faultId);
        this->seqCancelOut_out(0);
    } else {
        this->log_ACTIVITY_LO_ResponseQueued(entry.faultId);
    }
}

void FaultManager ::startNextResponse() {
    if (this->m_active >= 0 || this->m_inhibited) {
        return;
    }
    I32 next = -1;
    for (U16 i = 0; i < this->m_count; i++) {
        const Entry& entry = this->m_entries[i];
        if (entry.queued && entry.state == FaultState::ACTIVE &&
            (next < 0 || entry.priority > this->m_entries[next].priority)) {
            next = static_cast<I32>(i);
        }
    }
    if (next < 0) {
        return;
    }
    Entry& entry = this->m_entries[next];
    entry.queued = false;
    if (!this->isConnected_seqRunOut_OutputPort(0)) {
        this->log_WARNING_HI_ResponseFailed(entry.faultId, Fw::CmdResponse::EXECUTION_ERROR);
        return;
    }
    this->m_active = next;
    this->log_ACTIVITY_HI_ResponseStarted(entry.faultId, Fw::LogStringArg(entry.sequence));
    Svc::SeqArgs noArgs;
    this->seqRunOut_out(0, entry.sequence, noArgs);
}

void FaultManager ::resetRuntime(Entry& entry) {
    entry.consecutive = 0;
    entry.total = 0;
    entry.latched = false;
    entry.eventsSent = 0;
    entry.limitReported = false;
    entry.queued = false;
}

void FaultManager ::writeTelemetry() {
    FaultStatusTable table;
    for (U16 i = 0; i < FaultManagerCfg::MaxFaults; i++) {
        if (i < this->m_count) {
            const Entry& entry = this->m_entries[i];
            table[i] = FaultStatus(entry.faultId, entry.state, entry.consecutive, entry.total, entry.latched);
        } else {
            table[i] = FaultStatus(0, FaultState::DISABLED, 0, 0, false);
        }
    }
    this->tlmWrite_FaultTable(table);
    this->tlmWrite_ActiveResponseFault(this->m_active >= 0 ? this->m_entries[this->m_active].faultId : 0);
    this->tlmWrite_LastDeclaredFault(this->m_lastDeclared);
    this->tlmWrite_DeclarationCount(this->m_declarations);
    this->tlmWrite_ResponsesInhibited(this->m_inhibited);
}

}  // namespace Svc
