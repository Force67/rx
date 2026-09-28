#!/usr/bin/env python3
"""Check that the renderer gate cannot turn absent coverage into a pass."""

from pathlib import Path
import contextlib
import io
import os
import sys
import tempfile
import unittest

from check import report_failures, run_logged, selected_tests


class ReportTest(unittest.TestCase):
    def check_report(self, xml, expected=("gpu",)):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text(xml)
            return report_failures(path, set(expected))

    def test_passed(self):
        self.assertEqual(self.check_report(
            '<testsuite><testcase name="gpu" status="run"/></testsuite>'), [])

    def test_skipped(self):
        failures = self.check_report(
            '<testsuite><testcase name="gpu" status="notrun"><skipped message="no GPU"/>'
            '</testcase></testsuite>')
        self.assertIn("required coverage skipped: gpu", failures)

    def test_failure(self):
        self.assertIn("test failed: gpu", self.check_report(
            '<testsuite><testcase name="gpu" status="run"><failure/></testcase></testsuite>'))

    def test_exit_zero_skip_diagnostic(self):
        self.assertIn("required coverage skipped: gpu", self.check_report(
            '<testsuite><testcase name="gpu" status="run"><system-out>'
            'spatial checks SKIP, ray queries unavailable</system-out></testcase></testsuite>'))

    def test_skipped_frame_reset_is_not_a_skip(self):
        self.assertEqual(self.check_report(
            '<testsuite><testcase name="gpu" status="run"><system-out>'
            'skipped-frame reset MSE=0</system-out></testcase></testsuite>'), [])

    def test_missing(self):
        self.assertIn("missing result: gpu", self.check_report('<testsuite/>'))

    def test_incomplete_execution(self):
        self.assertIn("test did not report execution: gpu", self.check_report(
            '<testsuite><testcase name="gpu" status="fail"/></testsuite>'))

    def test_duplicate(self):
        self.assertIn("duplicate result: gpu", self.check_report(
            '<testsuite><testcase name="gpu" status="run"/>'
            '<testcase name="gpu" status="run"/></testsuite>'))

    def test_wrong_selection(self):
        failures = self.check_report('<testsuite><testcase name="cpu" status="run"/></testsuite>')
        self.assertIn("missing result: gpu", failures)
        self.assertIn("unexpected result: cpu", failures)

    def test_malformed(self):
        self.assertTrue(self.check_report('<testsuite>'))

    def test_missing_report(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(report_failures(Path(directory) / "absent.xml", {"gpu"}))

    def test_profiles_deduplicate(self):
        self.assertEqual(selected_tests(["portable", "portable"]), selected_tests(["portable"]))
        self.assertIn("post_sampling_test", selected_tests(["portable"]))
        self.assertIn("upscaler_motion_test_dlss", selected_tests(["dlss"]))

    def test_timeout_fails_and_keeps_log(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "process.log"
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                result = run_logged([sys.executable, "-c", "import time; time.sleep(30)"],
                                    path, dict(os.environ), timeout=1)
            self.assertNotEqual(result, 0)
            self.assertIn("timed out", path.read_text())


if __name__ == "__main__":
    unittest.main()
