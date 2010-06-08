//===-- ThreadPlanRunToAddress.h --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ThreadPlanRunToAddress_h_
#define liblldb_ThreadPlanRunToAddress_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Target/ThreadPlan.h"

namespace lldb_private {

class ThreadPlanRunToAddress : public ThreadPlan
{
public:
    ThreadPlanRunToAddress (Thread &thread,
                            Address &address,
                            bool stop_others);

    ThreadPlanRunToAddress (Thread &thread,
                            lldb::addr_t address,
                            bool stop_others);

    virtual
    ~ThreadPlanRunToAddress ();

    virtual void
    GetDescription (Stream *s, lldb::DescriptionLevel level);

    virtual bool
    ValidatePlan (Stream *error);

    virtual bool
    PlanExplainsStop ();

    virtual bool
    ShouldStop (Event *event_ptr);

    virtual bool
    StopOthers ();
    
    virtual void
    SetStopOthers (bool new_value);
    
    virtual lldb::StateType
    RunState ();

    virtual bool
    WillStop ();

    virtual bool
    MischiefManaged ();

protected:
    void SetInitialBreakpoint();
    bool AtOurAddress();
private:
    bool m_stop_others;
    lldb::addr_t m_address;   // This is the address we are going to run to.
                          // TODO: Would it be useful to have multiple addresses?
    lldb::user_id_t m_break_id; // This is the breakpoint we are using to stop us at m_address.

    DISALLOW_COPY_AND_ASSIGN (ThreadPlanRunToAddress);

};

} // namespace lldb_private

#endif  // liblldb_ThreadPlanRunToAddress_h_
