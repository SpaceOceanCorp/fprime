// ======================================================================
// \title  FaultManagerTester.cpp
// \brief  cpp file for FaultManager component test harness implementation class
// ======================================================================

#include "FaultManagerTester.hpp"
#include <cstdio>
#include <cstring>
#include "Os/File.hpp"
#include "Utils/Hash/Hash.hpp"

namespace Svc {

namespace {
const char* const TABLE_FILE = "fault_manager_ut_table.bin";

void putU32(std::vector<U8>& b, U32 v) {
    b.push_back(static_cast<U8>(v >> 24));
    b.push_back(static_cast<U8>(v >> 16));
    b.push_back(static_cast<U8>(v >> 8));
    b.push_back(static_cast<U8>(v));
}
}  // namespace

// ----------------------------------------------------------------------
// Construction and destruction
// ----------------------------------------------------------------------

FaultManagerTester ::FaultManagerTester()
    : FaultManagerGTestBase("FaultManagerTester", FaultManagerTester::MAX_HISTORY_SIZE), component("FaultManager") {
    this->initComponents();
    this->connectPorts();
}

FaultManagerTester ::~FaultManagerTester() {
    this->component.deinit();
    (void)std::remove(TABLE_FILE);
}

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

std::vector<U8> FaultManagerTester ::makeTable(const std::vector<TestEntry>& entries, bool corruptCrc) {
    std::vector<U8> b;
    putU32(b, FaultManager::TABLE_MAGIC);
    b.push_back(static_cast<U8>(entries.size() >> 8));
    b.push_back(static_cast<U8>(entries.size()));
    for (const TestEntry& e : entries) {
        putU32(b, e.faultId);
        b.push_back(e.state);
        b.push_back(e.persistence);
        b.push_back(e.flags);
        b.push_back(e.priority);
        b.push_back(e.eventLimit);
        const size_t n = std::strlen(e.sequence);
        b.push_back(static_cast<U8>(n));
        for (size_t i = 0; i < n; i++) {
            b.push_back(static_cast<U8>(e.sequence[i]));
        }
    }
    Utils::Hash hash;
    hash.init();
    hash.update(b.data(), static_cast<FwSizeType>(b.size()));
    U32 crc = 0;
    hash.finalize(crc);
    putU32(b, corruptCrc ? (crc ^ 1U) : crc);
    return b;
}

void FaultManagerTester ::writeFile(const char* fileName, const std::vector<U8>& bytes) {
    Os::File file;
    ASSERT_EQ(file.open(fileName, Os::File::OPEN_CREATE, Os::File::OVERWRITE), Os::File::OP_OK);
    FwSizeType size = static_cast<FwSizeType>(bytes.size());
    ASSERT_EQ(file.write(bytes.data(), size, Os::File::WAIT), Os::File::OP_OK);
    ASSERT_EQ(size, static_cast<FwSizeType>(bytes.size()));
    file.close();
}

void FaultManagerTester ::loadStandardTable() {
    const U8 latchClear = FaultManager::FLAG_LATCH | FaultManager::FLAG_CLEAR_ON_SUCCESS;
    writeFile(TABLE_FILE, makeTable({
                              {100, FaultState::ACTIVE, 3, latchClear, 1, 0, "seq_low.bin"},
                              {200, FaultState::ACTIVE, 1, FaultManager::FLAG_LATCH, 5, 0, "seq_high.bin"},
                              {300, FaultState::PASSIVE, 2, 0, 0, 2, "seq_passive.bin"},
                              {400, FaultState::DISABLED, 1, 0, 0, 0, "seq_off.bin"},
                          }));
    ASSERT_TRUE(this->component.configure(TABLE_FILE));
    ASSERT_EVENTS_TableLoaded_SIZE(1);
    ASSERT_EVENTS_TableLoaded(0, TABLE_FILE, 4);
    this->clearHistory();
}

void FaultManagerTester ::announce(U32 faultId, U32 times, U32 data) {
    for (U32 i = 0; i < times; i++) {
        this->invoke_to_faultIn(0, faultId, data);
        this->component.doDispatch();
    }
}

void FaultManagerTester ::sequenceDone(Fw::CmdResponse::T response) {
    this->invoke_to_seqDoneIn(0, 0, 0, response);
    this->component.doDispatch();
}

// ----------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------

void FaultManagerTester ::testTableLoad() {
    this->loadStandardTable();
    // Reload the same table by command
    this->sendCmd_TABLE_RELOAD(0, 7, Fw::CmdStringArg(TABLE_FILE));
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE_SIZE(1);
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_TABLE_RELOAD, 7, Fw::CmdResponse::OK);
    ASSERT_EVENTS_TableLoaded_SIZE(1);
}

void FaultManagerTester ::testTableRejects() {
    this->loadStandardTable();
    struct Case {
        std::vector<U8> bytes;
        FaultTableError::T error;
    };
    std::vector<Case> cases;
    cases.push_back({makeTable({{1, 0, 1, 0, 0, 0, "a"}}, true), FaultTableError::BAD_CRC});
    {
        std::vector<U8> b = makeTable({{1, 0, 1, 0, 0, 0, "a"}});
        b[0] ^= 0xFF;  // magic, CRC is checked after the magic
        cases.push_back({b, FaultTableError::BAD_MAGIC});
    }
    cases.push_back({makeTable({{1, 0, 0, 0, 0, 0, "a"}}), FaultTableError::BAD_ENTRY});     // zero persistence
    cases.push_back({makeTable({{1, 3, 1, 0, 0, 0, "a"}}), FaultTableError::BAD_ENTRY});     // bad state
    cases.push_back({makeTable({{1, 0, 1, 0x80, 0, 0, "a"}}), FaultTableError::BAD_ENTRY});  // unknown flag
    cases.push_back({makeTable({{1, 0, 1, 0, 0, 0, ""}}), FaultTableError::BAD_ENTRY});      // empty name
    cases.push_back({makeTable({{1, 0, 1, 0, 0, 0, "a"}, {1, 0, 1, 0, 0, 0, "b"}}), FaultTableError::DUPLICATE_ID});
    {
        std::vector<TestEntry> many(FaultManagerCfg::MaxFaults + 1, {0, 0, 1, 0, 0, 0, "s"});
        for (U32 i = 0; i < many.size(); i++) {
            many[i].faultId = i + 1;
        }
        cases.push_back({makeTable(many), FaultTableError::TOO_MANY_ENTRIES});
    }
    {
        std::vector<U8> b = makeTable({{1, 0, 1, 0, 0, 0, "a"}});
        b.resize(5);  // shorter than header + CRC
        cases.push_back({b, FaultTableError::BAD_SIZE});
    }
    for (const Case& c : cases) {
        this->clearHistory();
        writeFile(TABLE_FILE, c.bytes);
        this->sendCmd_TABLE_RELOAD(0, 1, Fw::CmdStringArg(TABLE_FILE));
        this->component.doDispatch();
        ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_TABLE_RELOAD, 1, Fw::CmdResponse::EXECUTION_ERROR);
        ASSERT_EVENTS_TableRejected_SIZE(1);
        ASSERT_EVENTS_TableRejected(0, TABLE_FILE, c.error);
    }
    // Missing file
    this->clearHistory();
    this->sendCmd_TABLE_RELOAD(0, 1, Fw::CmdStringArg("no_such_fault_table.bin"));
    this->component.doDispatch();
    ASSERT_EVENTS_TableRejected(0, "no_such_fault_table.bin", FaultTableError::OPEN_FAILED);
    // The original table is still in use
    this->clearHistory();
    this->announce(200, 1);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);
}

void FaultManagerTester ::testPersistenceAndResponse() {
    this->loadStandardTable();
    this->announce(100, 2, 42);
    ASSERT_EVENTS_FaultDeclared_SIZE(0);
    ASSERT_from_seqRunOut_SIZE(0);
    this->announce(100, 1, 42);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_EVENTS_FaultDeclared(0, 100, 42, FaultState::ACTIVE);
    ASSERT_EVENTS_ResponseStarted(0, 100, "seq_low.bin");
    ASSERT_from_seqRunOut_SIZE(1);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(0).filename, Fw::String("seq_low.bin"));
    ASSERT_TLM_DeclarationCount(this->tlmHistory_DeclarationCount->size() - 1, 1);
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_EVENTS_ResponseCompleted_SIZE(1);
    ASSERT_EVENTS_ResponseCompleted(0, 100);
}

void FaultManagerTester ::testClearResetsPersistence() {
    this->loadStandardTable();
    this->announce(100, 2);
    this->invoke_to_faultClearIn(0, 100);
    this->component.doDispatch();
    this->announce(100, 2);
    ASSERT_EVENTS_FaultDeclared_SIZE(0);
    this->announce(100, 1);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
}

void FaultManagerTester ::testPassiveAndDisabled() {
    this->loadStandardTable();
    this->announce(300, 2, 9);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_EVENTS_FaultDeclared(0, 300, 9, FaultState::PASSIVE);
    ASSERT_from_seqRunOut_SIZE(0);
    this->announce(400, 5);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(0);
}

void FaultManagerTester ::testLatchAndReset() {
    this->loadStandardTable();
    this->announce(200, 1);
    this->sequenceDone(Fw::CmdResponse::OK);  // 200 does not clear on success
    this->clearHistory();
    this->announce(200, 3);  // latched: counted only
    ASSERT_EVENTS_FaultDeclared_SIZE(0);
    ASSERT_from_seqRunOut_SIZE(0);
    this->sendCmd_FAULT_RESET(0, 3, 200);
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_FAULT_RESET, 3, Fw::CmdResponse::OK);
    ASSERT_EVENTS_FaultReset(0, 200);
    this->announce(200, 1);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);
}

void FaultManagerTester ::testClearOnSuccess() {
    this->loadStandardTable();
    this->announce(100, 3);
    this->sequenceDone(Fw::CmdResponse::OK);
    this->clearHistory();
    this->announce(100, 3);  // latch was cleared, so it can be declared again
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);
}

void FaultManagerTester ::testResponseFailureKeepsLatch() {
    this->loadStandardTable();
    this->announce(100, 3);
    this->sequenceDone(Fw::CmdResponse::EXECUTION_ERROR);
    ASSERT_EVENTS_ResponseFailed_SIZE(1);
    ASSERT_EVENTS_ResponseFailed(0, 100, Fw::CmdResponse::EXECUTION_ERROR);
    this->clearHistory();
    this->announce(100, 3);
    ASSERT_EVENTS_FaultDeclared_SIZE(0);
    ASSERT_from_seqRunOut_SIZE(0);
}

void FaultManagerTester ::testPriorityPreemption() {
    this->loadStandardTable();
    this->announce(100, 3);  // low priority starts
    ASSERT_from_seqRunOut_SIZE(1);
    this->announce(200, 1);  // high priority preempts
    ASSERT_EVENTS_ResponsePreempted_SIZE(1);
    ASSERT_EVENTS_ResponsePreempted(0, 100, 200);
    ASSERT_from_seqCancelOut_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);                         // waits for the cancel to complete
    this->sequenceDone(Fw::CmdResponse::EXECUTION_ERROR);  // the canceled sequence ends
    ASSERT_EVENTS_ResponseFailed_SIZE(0);
    ASSERT_from_seqRunOut_SIZE(2);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(1).filename, Fw::String("seq_high.bin"));
}

void FaultManagerTester ::testLowerPriorityQueued() {
    this->loadStandardTable();
    this->announce(200, 1);  // high priority runs
    this->announce(100, 3);  // low priority waits
    ASSERT_EVENTS_ResponseQueued_SIZE(1);
    ASSERT_EVENTS_ResponseQueued(0, 100);
    ASSERT_from_seqCancelOut_SIZE(0);
    ASSERT_from_seqRunOut_SIZE(1);
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_from_seqRunOut_SIZE(2);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(1).filename, Fw::String("seq_low.bin"));
}

void FaultManagerTester ::testInhibit() {
    this->loadStandardTable();
    this->sendCmd_RESPONSES_INHIBIT(0, 1);
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_RESPONSES_INHIBIT, 1, Fw::CmdResponse::OK);
    ASSERT_EVENTS_ResponsesInhibitSet(0, true);
    this->announce(200, 1);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_EVENTS_ResponseInhibited_SIZE(1);
    ASSERT_EVENTS_ResponseInhibited(0, 200);
    ASSERT_from_seqRunOut_SIZE(0);
    this->sendCmd_RESPONSES_ALLOW(0, 2);
    this->component.doDispatch();
    ASSERT_EVENTS_ResponsesInhibitSet(1, false);
    ASSERT_from_seqRunOut_SIZE(0);  // an inhibited declaration is not run later
}

void FaultManagerTester ::testEventLimit() {
    this->loadStandardTable();
    this->announce(300, 2 * 5);  // passive, persistence 2, limit 2 events: 5 declarations
    ASSERT_EVENTS_FaultDeclared_SIZE(2);
    ASSERT_EVENTS_EventLimitReached_SIZE(1);
    ASSERT_EVENTS_EventLimitReached(0, 300);
    ASSERT_TLM_DeclarationCount(this->tlmHistory_DeclarationCount->size() - 1, 5);
    this->sendCmd_FAULT_RESET(0, 1, 300);
    this->component.doDispatch();
    this->clearHistory();
    this->announce(300, 2);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
}

void FaultManagerTester ::testUnknownFault() {
    this->loadStandardTable();
    this->announce(999, 1);
    ASSERT_EVENTS_UnknownFault_SIZE(1);
    ASSERT_EVENTS_UnknownFault(0, 999);
    this->invoke_to_faultClearIn(0, 999);
    this->component.doDispatch();
    ASSERT_EVENTS_UnknownFault_SIZE(2);
    this->sendCmd_FAULT_RESET(0, 1, 999);
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_FAULT_RESET, 1, Fw::CmdResponse::VALIDATION_ERROR);
    this->clearHistory();
    this->sequenceDone(Fw::CmdResponse::OK);  // nothing running
    ASSERT_EVENTS_UnexpectedSequenceDone_SIZE(1);
}

void FaultManagerTester ::testReloadBusy() {
    this->loadStandardTable();
    this->announce(200, 1);
    this->sendCmd_TABLE_RELOAD(0, 4, Fw::CmdStringArg(TABLE_FILE));
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_TABLE_RELOAD, 4, Fw::CmdResponse::EXECUTION_ERROR);
    ASSERT_EVENTS_TableReloadBusy_SIZE(1);
}

void FaultManagerTester ::testSetStateCommand() {
    this->loadStandardTable();
    this->sendCmd_FAULT_SET_STATE(0, 5, 200, FaultState::DISABLED);
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(0, FaultManager::OPCODE_FAULT_SET_STATE, 5, Fw::CmdResponse::OK);
    ASSERT_EVENTS_FaultStateSet(0, 200, FaultState::DISABLED);
    this->announce(200, 3);
    ASSERT_EVENTS_FaultDeclared_SIZE(0);
    this->sendCmd_FAULT_SET_STATE(0, 6, 400, FaultState::ACTIVE);
    this->component.doDispatch();
    this->announce(400, 1);
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(0).filename, Fw::String("seq_off.bin"));
    this->sendCmd_FAULT_SET_STATE(0, 7, 999, FaultState::ACTIVE);
    this->component.doDispatch();
    ASSERT_CMD_RESPONSE(2, FaultManager::OPCODE_FAULT_SET_STATE, 7, Fw::CmdResponse::VALIDATION_ERROR);
}

void FaultManagerTester ::testPing() {
    this->invoke_to_pingIn(0, 0x1234);
    this->component.doDispatch();
    ASSERT_from_pingOut_SIZE(1);
    ASSERT_from_pingOut(0, 0x1234);
}

void FaultManagerTester ::testFullTable() {
    std::vector<TestEntry> all(FaultManagerCfg::MaxFaults, {0, FaultState::ACTIVE, 1, 0, 0, 0, "s"});
    for (U32 i = 0; i < all.size(); i++) {
        all[i].faultId = 1000 + i;
        all[i].priority = static_cast<U8>(i);
    }
    writeFile(TABLE_FILE, makeTable(all));
    ASSERT_TRUE(this->component.configure(TABLE_FILE));
    ASSERT_EVENTS_TableLoaded(0, TABLE_FILE, FaultManagerCfg::MaxFaults);
    this->clearHistory();
    this->announce(1000 + FaultManagerCfg::MaxFaults - 1, 1);  // last entry is found
    ASSERT_EVENTS_FaultDeclared_SIZE(1);
    ASSERT_from_seqRunOut_SIZE(1);
}

void FaultManagerTester ::testOversizeFile() {
    this->loadStandardTable();
    std::vector<U8> big(FaultManager::MAX_TABLE_SIZE + 1, 0);
    writeFile(TABLE_FILE, big);
    this->sendCmd_TABLE_RELOAD(0, 1, Fw::CmdStringArg(TABLE_FILE));
    this->component.doDispatch();
    ASSERT_EVENTS_TableRejected(0, TABLE_FILE, FaultTableError::BAD_SIZE);
    // Trailing bytes after the last entry are rejected too
    std::vector<U8> t = makeTable({{1, 0, 1, 0, 0, 0, "a"}});
    t.insert(t.end() - 4, 0x00);
    writeFile(TABLE_FILE, t);
    this->sendCmd_TABLE_RELOAD(0, 2, Fw::CmdStringArg(TABLE_FILE));
    this->component.doDispatch();
    ASSERT_EVENTS_TableRejected_SIZE(2);
    ASSERT_EVENTS_TableRejected(1, TABLE_FILE, FaultTableError::BAD_CRC);
}

void FaultManagerTester ::testPreemptWhileCancelPending() {
    const U8 latch = FaultManager::FLAG_LATCH;
    writeFile(TABLE_FILE, makeTable({
                              {1, FaultState::ACTIVE, 1, latch, 1, 0, "p1.bin"},
                              {2, FaultState::ACTIVE, 1, latch, 5, 0, "p5.bin"},
                              {3, FaultState::ACTIVE, 1, latch, 9, 0, "p9.bin"},
                          }));
    ASSERT_TRUE(this->component.configure(TABLE_FILE));
    this->clearHistory();
    this->announce(1, 1);
    this->announce(2, 1);  // preempts 1, cancel pending
    this->announce(3, 1);  // queued behind the pending cancel, no second cancel
    ASSERT_from_seqCancelOut_SIZE(1);
    ASSERT_EVENTS_ResponseQueued_SIZE(1);
    this->sequenceDone(Fw::CmdResponse::EXECUTION_ERROR);  // canceled p1 ends
    ASSERT_from_seqRunOut_SIZE(2);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(1).filename, Fw::String("p9.bin"));  // highest priority first
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_from_seqRunOut_SIZE(3);
    ASSERT_EQ(this->fromPortHistory_seqRunOut->at(2).filename, Fw::String("p5.bin"));
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_from_seqRunOut_SIZE(3);
    ASSERT_TLM_ActiveResponseFault(this->tlmHistory_ActiveResponseFault->size() - 1, 0);
}

void FaultManagerTester ::testSetStateDropsQueued() {
    this->loadStandardTable();
    this->announce(200, 1);  // runs
    this->announce(100, 3);  // queued
    this->sendCmd_FAULT_SET_STATE(0, 1, 100, FaultState::PASSIVE);
    this->component.doDispatch();
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_from_seqRunOut_SIZE(1);  // the queued response was dropped
}

void FaultManagerTester ::testTelemetryTable() {
    this->loadStandardTable();
    this->announce(100, 2);
    const FaultStatusTable& t = this->tlmHistory_FaultTable->at(this->tlmHistory_FaultTable->size() - 1).arg;
    ASSERT_EQ(t[0].get_faultId(), 100U);
    ASSERT_EQ(t[0].get_consecutive(), 2);
    ASSERT_EQ(t[0].get_total(), 2);
    ASSERT_FALSE(t[0].get_latched());
    ASSERT_EQ(t[3].get_state(), FaultState::DISABLED);
    ASSERT_EQ(t[4].get_faultId(), 0U);  // unused slot
}

std::vector<U8> FaultManagerTester ::withCrc(std::vector<U8> body) {
    Utils::Hash hash;
    hash.init();
    hash.update(body.data(), static_cast<FwSizeType>(body.size()));
    U32 crc = 0;
    hash.finalize(crc);
    putU32(body, crc);
    return body;
}

void FaultManagerTester ::testStructuralRejects() {
    this->loadStandardTable();
    // Header + count, built by hand so the CRC is valid but the structure is not
    const std::vector<U8> header = {0x46, 0x4D, 0x54, 0x31, 0x00, 0x01};
    struct Case {
        std::vector<U8> body;
        FaultTableError::T error;
    };
    std::vector<Case> cases;
    // Count says 1 entry but the fixed part is truncated
    cases.push_back({{0, 0, 0, 1, 0, 1}, FaultTableError::BAD_SIZE});
    // Name length runs past the end
    cases.push_back({{0, 0, 0, 1, 0, 1, 0, 0, 0, 5, 'a', 'b'}, FaultTableError::BAD_SIZE});
    // NUL inside the name
    cases.push_back({{0, 0, 0, 1, 0, 1, 0, 0, 0, 2, 'a', 0}, FaultTableError::BAD_ENTRY});
    // Valid entry followed by a trailing byte
    cases.push_back({{0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 'a', 0x99}, FaultTableError::BAD_SIZE});
    for (const Case& c : cases) {
        std::vector<U8> body = header;
        body.insert(body.end(), c.body.begin(), c.body.end());
        writeFile(TABLE_FILE, withCrc(body));
        this->clearHistory();
        this->sendCmd_TABLE_RELOAD(0, 1, Fw::CmdStringArg(TABLE_FILE));
        this->component.doDispatch();
        ASSERT_EVENTS_TableRejected_SIZE(1);
        ASSERT_EVENTS_TableRejected(0, TABLE_FILE, c.error);
    }
}

void FaultManagerTester ::testRedeclareWhileRunning() {
    writeFile(TABLE_FILE, makeTable({{9, FaultState::ACTIVE, 1, 0, 1, 0, "s9.bin"}}));
    ASSERT_TRUE(this->component.configure(TABLE_FILE));
    this->clearHistory();
    this->announce(9, 2);  // not latched: declared twice, but its response is already running
    ASSERT_EVENTS_FaultDeclared_SIZE(2);
    ASSERT_from_seqRunOut_SIZE(1);
    ASSERT_EVENTS_ResponseQueued_SIZE(0);
    this->sequenceDone(Fw::CmdResponse::OK);
    ASSERT_from_seqRunOut_SIZE(1);  // not run again
}

}  // namespace Svc
