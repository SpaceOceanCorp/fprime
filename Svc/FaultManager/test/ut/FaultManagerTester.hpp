// ======================================================================
// \title  FaultManagerTester.hpp
// \brief  hpp file for FaultManager component test harness implementation class
// ======================================================================

#ifndef Svc_FaultManagerTester_HPP
#define Svc_FaultManagerTester_HPP

#include <vector>
#include "Svc/FaultManager/FaultManager.hpp"
#include "Svc/FaultManager/FaultManagerGTestBase.hpp"

namespace Svc {

class FaultManagerTester final : public FaultManagerGTestBase {
  public:
    static const FwSizeType MAX_HISTORY_SIZE = 64;
    static const FwEnumStoreType TEST_INSTANCE_ID = 0;
    static const FwSizeType TEST_INSTANCE_QUEUE_DEPTH = 32;

    //! One entry of a test table
    struct TestEntry {
        U32 faultId;
        U8 state;
        U8 persistence;
        U8 flags;
        U8 priority;
        U8 eventLimit;
        const char* sequence;
    };

    FaultManagerTester();
    ~FaultManagerTester();

    // Tests
    void testTableLoad();
    void testGeneratorGoldenTable();
    void testNameLengthBoundary();
    void testTableRejects();
    void testPersistenceAndResponse();
    void testClearResetsPersistence();
    void testPassiveAndDisabled();
    void testLatchAndReset();
    void testClearOnSuccess();
    void testResponseFailureKeepsLatch();
    void testPriorityPreemption();
    void testLowerPriorityQueued();
    void testInhibit();
    void testEventLimit();
    void testUnknownFault();
    void testReloadBusy();
    void testSetStateCommand();
    void testPing();
    void testFullTable();
    void testOversizeFile();
    void testPreemptWhileCancelPending();
    void testSetStateDropsQueued();
    void testTelemetryTable();
    void testStructuralRejects();
    void testRedeclareWhileRunning();

  private:
    //! Serialize a table image (with a correct CRC unless \p corruptCrc)
    static std::vector<U8> makeTable(const std::vector<TestEntry>& entries, bool corruptCrc = false);
    //! Write bytes to \p fileName
    static void writeFile(const char* fileName, const std::vector<U8>& bytes);
    //! Append a CRC32 to \p body
    static std::vector<U8> withCrc(std::vector<U8> body);
    //! Load a standard three-fault table through configure()
    void loadStandardTable();
    //! Announce \p faultId \p times times and dispatch each message
    void announce(U32 faultId, U32 times, U32 data = 0);
    //! Report the running sequence done with \p response
    void sequenceDone(Fw::CmdResponse::T response);

    void connectPorts();
    void initComponents();

    FaultManager component;
};

}  // namespace Svc

#endif
