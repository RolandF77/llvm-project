"""
Test lldb-dap disconnect request
"""


import dap_server
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
import lldbdap_testcase
import subprocess
import time
import os


class TestDAP_launch(lldbdap_testcase.DAPTestCaseBase):
    source = "main.cpp"

    def disconnect_and_assert_no_output_printed(self):
        self.dap_server.request_disconnect()
        # verify we didn't get any input after disconnect
        time.sleep(2)
        output = self.get_stdout()
        self.assertTrue(output is None or len(output) == 0)

    @skipIfWindows
    def test_launch(self):
        """
        This test launches a process that would creates a file, but we disconnect
        before the file is created, which terminates the process and thus the file is not
        created.
        """
        program = self.getBuildArtifact("a.out")
        self.build_and_launch(program, stopOnEntry=True, disconnectAutomatically=False)

        # We set a breakpoint right before the side effect file is created
        self.set_source_breakpoints(
            self.source, [line_number(self.source, "// breakpoint")]
        )
        self.continue_to_next_stop()

        # verify we haven't produced the side effect file yet
        self.assertFalse(os.path.exists(program + ".side_effect"))

        self.dap_server.request_disconnect()

        # verify we didn't produce the side effect file
        time.sleep(1)
        self.assertFalse(os.path.exists(program + ".side_effect"))

    @skipIfWindows
    @expectedFailureNetBSD
    def test_attach(self):
        """
        This test attaches to a process that creates a file. We attach and disconnect
        before the file is created, and as the process is not terminated upon disconnection,
        the file is created anyway.
        """
        self.build_and_create_debug_adapter()
        program = self.getBuildArtifact("a.out")

        # Use a file as a synchronization point between test and inferior.
        sync_file_path = lldbutil.append_to_process_working_directory(
            self, "sync_file_%d" % (int(time.time()))
        )
        self.addTearDownHook(
            lambda: self.run_platform_command("rm %s" % (sync_file_path))
        )

        self.process = subprocess.Popen([program, sync_file_path])
        lldbutil.wait_for_file_on_target(self, sync_file_path)

        self.attach(pid=self.process.pid, disconnectAutomatically=False)
        response = self.dap_server.request_evaluate("wait_for_attach = false;")
        self.assertTrue(response["success"])

        # verify we haven't produced the side effect file yet
        self.assertFalse(os.path.exists(program + ".side_effect"))

        self.dap_server.request_disconnect()
        time.sleep(2)
        # verify we produced the side effect file, as the program continued after disconnecting
        self.assertTrue(os.path.exists(program + ".side_effect"))
