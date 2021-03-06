"""
Test breakpoint conditions with 'breakpoint modify -c <expr> id'.
"""

import os, time
import re
import unittest2
import lldb, lldbutil
from lldbtest import *

class BreakpointConditionsTestCase(TestBase):

    mydir = os.path.join("functionalities", "breakpoint", "breakpoint_conditions")

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    @dsym_test
    def test_breakpoint_condition_with_dsym_and_run_command(self):
        """Exercise breakpoint condition with 'breakpoint modify -c <expr> id'."""
        self.buildDsym()
        self.breakpoint_conditions()

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    @dsym_test
    def test_breakpoint_condition_inline_with_dsym_and_run_command(self):
        """Exercise breakpoint condition inline with 'breakpoint set'."""
        self.buildDsym()
        self.breakpoint_conditions(inline=True)

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    @python_api_test
    @dsym_test
    def test_breakpoint_condition_with_dsym_and_python_api(self):
        """Use Python APIs to set breakpoint conditions."""
        self.buildDsym()
        self.breakpoint_conditions_python()

    @expectedFailureFreeBSD('llvm.org/pr17213')
    @dwarf_test
    def test_breakpoint_condition_with_dwarf_and_run_command(self):
        """Exercise breakpoint condition with 'breakpoint modify -c <expr> id'."""
        self.buildDwarf()
        self.breakpoint_conditions()

    @expectedFailureFreeBSD('llvm.org/pr17213')
    @dwarf_test
    def test_breakpoint_condition_inline_with_dwarf_and_run_command(self):
        """Exercise breakpoint condition inline with 'breakpoint set'."""
        self.buildDwarf()
        self.breakpoint_conditions(inline=True)

    @python_api_test
    @dwarf_test
    def test_breakpoint_condition_with_dwarf_and_python_api(self):
        """Use Python APIs to set breakpoint conditions."""
        self.buildDwarf()
        self.breakpoint_conditions_python()

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)
        # Find the line number to of function 'c'.
        self.line1 = line_number('main.c', '// Find the line number of function "c" here.')
        self.line2 = line_number('main.c', "// Find the line number of c's parent call here.")

    def breakpoint_conditions(self, inline=False):
        """Exercise breakpoint condition with 'breakpoint modify -c <expr> id'."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        if inline:
            # Create a breakpoint by function name 'c' and set the condition.
            lldbutil.run_break_set_by_symbol (self, "c", extra_options="-c 'val == 3'", num_expected_locations=1, sym_exact=True)
        else:
            # Create a breakpoint by function name 'c'.
            lldbutil.run_break_set_by_symbol (self, "c", num_expected_locations=1, sym_exact=True)

            # And set a condition on the breakpoint to stop on when 'val == 3'.
            self.runCmd("breakpoint modify -c 'val == 3' 1")

        # Now run the program.
        self.runCmd("run", RUN_SUCCEEDED)

        # The process should be stopped at this point.
        self.expect("process status", PROCESS_STOPPED,
            patterns = ['Process .* stopped'])

        # 'frame variable --show-types val' should return 3 due to breakpoint condition.
        self.expect("frame variable --show-types val", VARIABLES_DISPLAYED_CORRECTLY,
            startstr = '(int) val = 3')

        # Also check the hit count, which should be 3, by design.
        self.expect("breakpoint list -f", BREAKPOINT_HIT_THRICE,
            substrs = ["resolved = 1",
                       "Condition: val == 3",
                       "hit count = 3"])

        # The frame #0 should correspond to main.c:36, the executable statement
        # in function name 'c'.  And the parent frame should point to main.c:24.
        self.expect("thread backtrace", STOPPED_DUE_TO_BREAKPOINT_CONDITION,
            #substrs = ["stop reason = breakpoint"],
            patterns = ["frame #0.*main.c:%d" % self.line1,
                        "frame #1.*main.c:%d" % self.line2])

        # Test that "breakpoint modify -c ''" clears the condition for the last
        # created breakpoint, so that when the breakpoint hits, val == 1.
        self.runCmd("process kill")
        self.runCmd("breakpoint modify -c ''")
        self.expect("breakpoint list -f", BREAKPOINT_STATE_CORRECT, matching=False,
            substrs = ["Condition:"])

        # Now run the program again.
        self.runCmd("run", RUN_SUCCEEDED)

        # The process should be stopped at this point.
        self.expect("process status", PROCESS_STOPPED,
            patterns = ['Process .* stopped'])

        # 'frame variable --show-types val' should return 1 since it is the first breakpoint hit.
        self.expect("frame variable --show-types val", VARIABLES_DISPLAYED_CORRECTLY,
            startstr = '(int) val = 1')

        self.runCmd("process kill")
        self.runCmd("breakpoint disable")

        self.runCmd("breakpoint set -p Loop")
        self.runCmd("breakpoint modify -c ($eax&&i)")
        self.runCmd("run")
        
        self.expect("process status", PROCESS_STOPPED,
            patterns = ['Process .* stopped'])

        self.runCmd("continue")

        self.expect("process status", PROCESS_EXITED,
            patterns = ['Process .* exited'])

    def breakpoint_conditions_python(self):
        """Use Python APIs to set breakpoint conditions."""
        exe = os.path.join(os.getcwd(), "a.out")

        # Create a target by the debugger.
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Now create a breakpoint on main.c by name 'c'.
        breakpoint = target.BreakpointCreateByName('c', 'a.out')
        #print "breakpoint:", breakpoint
        self.assertTrue(breakpoint and
                        breakpoint.GetNumLocations() == 1,
                        VALID_BREAKPOINT)

        # We didn't associate a thread index with the breakpoint, so it should be invalid.
        self.assertTrue(breakpoint.GetThreadIndex() == lldb.UINT32_MAX,
                        "The thread index should be invalid")
        # The thread name should be invalid, too.
        self.assertTrue(breakpoint.GetThreadName() is None,
                        "The thread name should be invalid")

        # Let's set the thread index for this breakpoint and verify that it is,
        # indeed, being set correctly.
        breakpoint.SetThreadIndex(1) # There's only one thread for the process.
        self.assertTrue(breakpoint.GetThreadIndex() == 1,
                        "The thread index has been set correctly")

        # Get the breakpoint location from breakpoint after we verified that,
        # indeed, it has one location.
        location = breakpoint.GetLocationAtIndex(0)
        self.assertTrue(location and
                        location.IsEnabled(),
                        VALID_BREAKPOINT_LOCATION)

        # Set the condition on the breakpoint location.
        location.SetCondition('val == 3')
        self.expect(location.GetCondition(), exe=False,
            startstr = 'val == 3')

        # Now launch the process, and do not stop at entry point.
        process = target.LaunchSimple(None, None, os.getcwd())
        self.assertTrue(process, PROCESS_IS_VALID)

        # Frame #0 should be on self.line1 and the break condition should hold.
        from lldbutil import get_stopped_thread
        thread = get_stopped_thread(process, lldb.eStopReasonBreakpoint)
        self.assertTrue(thread.IsValid(), "There should be a thread stopped due to breakpoint condition")
        frame0 = thread.GetFrameAtIndex(0)
        var = frame0.FindValue('val', lldb.eValueTypeVariableArgument)
        self.assertTrue(frame0.GetLineEntry().GetLine() == self.line1 and
                        var.GetValue() == '3')

        # The hit count for the breakpoint should be 3.
        self.assertTrue(breakpoint.GetHitCount() == 3)

        process.Continue()

        
if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
