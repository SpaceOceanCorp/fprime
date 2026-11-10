// ======================================================================
// \title  FaultManager.hpp
// \brief  hpp file for FaultManager component implementation class
// ======================================================================

#ifndef Svc_FaultManager_HPP
#define Svc_FaultManager_HPP

#include "Fw/Types/String.hpp"
#include "Svc/FaultManager/FaultManagerComponentAc.hpp"
#include "config/FppConstantsAc.hpp"

namespace Svc {

//! \class FaultManager
//! \brief Filters fault announcements and runs response sequences
//!
//! Each fault in a table loaded from a file has a state (ACTIVE, PASSIVE or DISABLED), a
//! persistence count, a latch, a response sequence, a priority and an event limit. A fault is
//! declared after its persistence count of consecutive announcements. An ACTIVE fault then runs
//! its response sequence; one response runs at a time, and a higher-priority response cancels a
//! lower one.
class FaultManager final : public FaultManagerComponentBase {
  public:
    //! Table file magic number ("FMT1")
    static constexpr U32 TABLE_MAGIC = 0x464D5431;
    //! Size of the fixed part of one table entry
    static constexpr FwSizeType ENTRY_FIXED_SIZE = 10;
    //! Size of the table header (magic, count)
    static constexpr FwSizeType HEADER_SIZE = 6;
    //! Size of the CRC32 trailer
    static constexpr FwSizeType CRC_SIZE = 4;
    //! Largest valid table file
    static constexpr FwSizeType MAX_TABLE_SIZE =
        HEADER_SIZE + FaultManagerCfg::MaxFaults * (ENTRY_FIXED_SIZE + FileNameStringSize) + CRC_SIZE;

    //! Entry flag: latch the fault after a declaration
    static constexpr U8 FLAG_LATCH = 0x01;
    //! Entry flag: clear the latch and counts when the response succeeds
    static constexpr U8 FLAG_CLEAR_ON_SUCCESS = 0x02;

    //! Construct FaultManager object
    explicit FaultManager(const char* const compName  //!< The component name
    );

    //! Destroy FaultManager object
    ~FaultManager();

    //! Load the initial fault table. Call after the component's ports are connected.
    //! \return true if the table was loaded
    bool configure(const char* tableFileName  //!< The fault table file
    );

  private:
    //! One fault table entry and its runtime state
    struct Entry {
        // From the table
        U32 faultId;
        FaultState state;
        U8 persistence;
        U8 flags;
        U8 priority;
        U8 eventLimit;  //!< 0 means no limit
        Fw::String sequence;
        // Runtime
        U8 consecutive;
        U16 total;
        bool latched;
        U8 eventsSent;
        bool limitReported;
        bool queued;
    };

    // ----------------------------------------------------------------------
    // Handler implementations for typed input ports
    // ----------------------------------------------------------------------

    void faultIn_handler(FwIndexType portNum, U32 faultId, U32 data) override;

    void faultClearIn_handler(FwIndexType portNum, U32 faultId) override;

    void seqDoneIn_handler(FwIndexType portNum,
                           FwOpcodeType opCode,
                           U32 cmdSeq,
                           const Fw::CmdResponse& response) override;

    void pingIn_handler(FwIndexType portNum, U32 key) override;

    // ----------------------------------------------------------------------
    // Handler implementations for commands
    // ----------------------------------------------------------------------

    void FAULT_SET_STATE_cmdHandler(FwOpcodeType opCode,
                                    U32 cmdSeq,
                                    U32 faultId,
                                    const Svc::FaultState& state) override;

    void FAULT_RESET_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, U32 faultId) override;

    void TABLE_RELOAD_cmdHandler(FwOpcodeType opCode, U32 cmdSeq, const Fw::CmdStringArg& fileName) override;

    void RESPONSES_INHIBIT_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) override;

    void RESPONSES_ALLOW_cmdHandler(FwOpcodeType opCode, U32 cmdSeq) override;

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------

    //! Read, validate and install a table. The table in use is unchanged on failure.
    bool loadTable(const Fw::StringBase& fileName);

    //! Validate a table image and fill \p entries. Sets \p error on failure.
    bool parseTable(const U8* data, FwSizeType size, Entry* entries, U16& count, FaultTableError& error) const;

    //! Index of the entry for \p faultId, or -1
    I32 findEntry(U32 faultId) const;

    //! Declare the fault at \p idx
    void declare(I32 idx, U32 data);

    //! Queue the response for the fault at \p idx, preempting a lower-priority one
    void requestResponse(I32 idx);

    //! Start the highest-priority queued response if none is running
    void startNextResponse();

    //! Clear the runtime state of one entry
    static void resetRuntime(Entry& entry);

    //! Write all telemetry
    void writeTelemetry();

    // ----------------------------------------------------------------------
    // Member variables
    // ----------------------------------------------------------------------

    Entry m_entries[FaultManagerCfg::MaxFaults];  //!< Fault table
    U16 m_count;                                  //!< Entries in use
    I32 m_active;                                 //!< Entry whose response is running, or -1
    bool m_cancelPending;                         //!< The running response was canceled
    bool m_inhibited;                             //!< Responses are inhibited
    U32 m_declarations;                           //!< Declarations since startup
    U32 m_lastDeclared;                           //!< Most recently declared fault
    U8 m_fileBuffer[MAX_TABLE_SIZE + 1];          //!< Table file image (one extra byte detects oversize files)
    Entry m_staging[FaultManagerCfg::MaxFaults];  //!< Table being validated
};

}  // namespace Svc

#endif
