// ======================================================================
// \title  FaultManagerTestMain.cpp
// \brief  cpp file for FaultManager component test main function
// ======================================================================

#include "FaultManagerTester.hpp"

#define FM_TEST(group, name)            \
    TEST(group, name) {                 \
        Svc::FaultManagerTester tester; \
        tester.name();                  \
    }

FM_TEST(Table, testTableLoad)
FM_TEST(Table, testTableRejects)
FM_TEST(Table, testReloadBusy)
FM_TEST(Nominal, testPersistenceAndResponse)
FM_TEST(Nominal, testClearResetsPersistence)
FM_TEST(Nominal, testPassiveAndDisabled)
FM_TEST(Latch, testLatchAndReset)
FM_TEST(Latch, testClearOnSuccess)
FM_TEST(Latch, testResponseFailureKeepsLatch)
FM_TEST(Priority, testPriorityPreemption)
FM_TEST(Priority, testLowerPriorityQueued)
FM_TEST(Commands, testInhibit)
FM_TEST(Commands, testSetStateCommand)
FM_TEST(OffNominal, testEventLimit)
FM_TEST(OffNominal, testUnknownFault)
FM_TEST(Health, testPing)
FM_TEST(Table, testFullTable)
FM_TEST(Table, testOversizeFile)
FM_TEST(Priority, testPreemptWhileCancelPending)
FM_TEST(Commands, testSetStateDropsQueued)
FM_TEST(Telemetry, testTelemetryTable)
FM_TEST(Table, testStructuralRejects)
FM_TEST(Priority, testRedeclareWhileRunning)

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
