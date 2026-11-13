"""Unit tests for tools/ci/annotate.py (CI annotations of failed checks)."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CI_DIR, "annotate.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)

import annotate  # noqa: E402

JUNIT_FAILED = """expected failure: gpu_tiler
retried: launcher_ui_library_search_timing_x11 (failed in the full run, worst median 120.0 ms; \
passed in the rerun, worst median 80.0 ms)
failed: kernel_file_system
skipped: launcher_ui_run_wayland (SKIP_RETURN_CODE=77)
not run: lld_linked_executables (Unable to find executable)
missing: docs_links
unexpected pass: gpu_command_lane (listed as an expected failure; remove it from the list)
bad rerun: ime_dialog (the rerun ran a test that the retry list lacks)
junit-check: 210 tests, 185 passed, 19 expected failures, 1 retried, 6 problems
"""

COVERAGE_OUTPUT = """scope line coverage: 48.74% (61234/125634)
PASS group launcher_core lines: measured 97.83% (5000/5111), threshold 90%
PASS directory src/common lines: measured 61.20% (100/163), must be above 0
FAIL directory src/loader lines: measured 0.00% (0/80), must be above 0
FAIL baseline .github/coverage-baseline.txt is missing; measured scope line coverage 48.74%
coverage check failed: 2 failures
"""

TITLE = "Fedora stage A: test results"


class Run(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.temp.name, "output.txt")

    def tearDown(self):
        self.temp.cleanup()

    def annotate(self, mode, text, title=TITLE):
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(text)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = annotate.main([mode, self.path, "--title", title])
        self.assertEqual(status, 0)
        return stdout.getvalue().splitlines()


class Escaping(unittest.TestCase):
    def test_data_escapes_percent_and_line_breaks(self):
        self.assertEqual(annotate.escape_data("50% a\r\nb: c, d"), "50%25 a%0D%0Ab: c, d")

    def test_properties_also_escape_colons_and_commas(self):
        self.assertEqual(annotate.escape_property("a: b, 5%\n"), "a%3A b%2C 5%25%0A")

    def test_the_title_and_message_are_escaped(self):
        self.assertEqual(annotate.command("error", TITLE, "x 1%"),
                         "::error title=Fedora stage A%3A test results::x 1%25")


class Junit(Run):
    def test_problem_retried_and_summary_lines(self):
        lines = self.annotate("junit", JUNIT_FAILED)
        title = "title=Fedora stage A%3A test results"
        self.assertEqual(lines, [
            f"::error {title}::failed: kernel_file_system",
            f"::error {title}::skipped: launcher_ui_run_wayland (SKIP_RETURN_CODE=77)",
            f"::error {title}::not run: lld_linked_executables (Unable to find executable)",
            f"::error {title}::missing: docs_links",
            f"::error {title}::unexpected pass: gpu_command_lane (listed as an expected failure; "
            "remove it from the list)",
            f"::error {title}::bad rerun: ime_dialog (the rerun ran a test that the retry list "
            "lacks)",
            f"::warning {title}::retried: launcher_ui_library_search_timing_x11 (failed in the "
            "full run, worst median 120.0 ms; passed in the rerun, worst median 80.0 ms)",
            f"::notice {title}::junit-check: 210 tests, 185 passed, 19 expected failures, "
            "1 retried, 6 problems",
        ])

    def test_expected_failures_are_not_annotated(self):
        lines = self.annotate("junit", "expected failure: gpu_tiler\n"
                              "junit-check: 1 tests, 0 passed, 1 expected failures, 0 retried, "
                              "0 problems\n")
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("::notice "), lines)

    def test_no_tests(self):
        lines = self.annotate("junit", "no tests: ctest.xml has no testcase\n"
                              "junit-check: 0 tests, 0 passed, 1 problems\n")
        self.assertEqual(lines[0], f"::error title={annotate.escape_property(TITLE)}::no tests: "
                         "ctest.xml has no testcase")

    def test_at_most_nine_errors_and_one_more_line(self):
        text = "".join(f"failed: test_{number:02}\n" for number in range(12))
        lines = self.annotate("junit", text + "junit-check: 12 tests, 0 passed, 12 problems\n")
        errors = [line for line in lines if line.startswith("::error ")]
        self.assertEqual(len(errors), 10)
        self.assertTrue(errors[8].endswith("::failed: test_08"), errors)
        self.assertTrue(errors[9].endswith("::and 3 more problems; see the job log"), errors)
        self.assertTrue(lines[-1].startswith("::notice "), lines)

    def test_output_without_a_summary_is_an_error(self):
        lines = self.annotate("junit", "")
        self.assertEqual(len(lines), 1)
        self.assertIn("printed no junit-check: summary", lines[0])
        self.assertTrue(lines[0].startswith("::error "), lines)


class Coverage(Run):
    def test_failures_and_the_measured_values(self):
        lines = self.annotate("coverage", COVERAGE_OUTPUT, title="Coverage check")
        self.assertEqual(lines, [
            "::error title=Coverage check::directory src/loader lines: measured 0.00% (0/80), "
            "must be above 0".replace("%", "%25"),
            "::error title=Coverage check::baseline .github/coverage-baseline.txt is missing; "
            "measured scope line coverage 48.74%25",
            "::notice title=Coverage check::scope line coverage 48.74%25; directory lines: "
            "src/common 61.20%25, src/loader 0.00%25",
        ])

    def test_a_passing_check_gives_only_the_notice(self):
        text = "\n".join(line for line in COVERAGE_OUTPUT.splitlines()
                         if not line.startswith("FAIL ")) + "\ncoverage check passed\n"
        lines = self.annotate("coverage", text, title="Coverage check")
        self.assertEqual(lines, ["::notice title=Coverage check::scope line coverage 48.74%25; "
                                 "directory lines: src/common 61.20%25"])

    def test_a_missing_summary(self):
        lines = self.annotate("coverage", "FAIL coverage summary s.json is missing\n"
                              "coverage check failed: 1 failures\n", title="Coverage check")
        self.assertEqual(lines, ["::error title=Coverage check::coverage summary s.json is "
                                 "missing"])

    def test_output_without_a_result_is_an_error(self):
        lines = self.annotate("coverage", "error: groups file g.json has no 'groups' object\n",
                              title="Coverage check")
        self.assertEqual(lines, ["::error title=Coverage check::check.py printed no result; see "
                                 "the job log"])


class Log(Run):
    def test_apt_dnf_ninja_and_compiler_errors(self):
        text = ("Reading package lists...\n"
                "E: Unable to locate package libfoo-dev\n"
                "Error: Unable to find a match: foo-devel\n"
                "[12/40] Building CXX object a.o\n"
                "FAILED: src/CMakeFiles/a.dir/a.cpp.o\n"
                "/src/a.cpp:3:5: error: use of undeclared identifier 'x'\n"
                "ninja: build stopped: subcommand failed.\n"
                "clang: warning: -Werror is set\n"
                "an_error: is not a word match; neither is my-error: here\n")
        lines = self.annotate("log", text, title="Ubuntu build")
        self.assertEqual(lines, [
            "::error title=Ubuntu build::E: Unable to locate package libfoo-dev",
            "::error title=Ubuntu build::Error: Unable to find a match: foo-devel",
            "::error title=Ubuntu build::FAILED: src/CMakeFiles/a.dir/a.cpp.o",
            "::error title=Ubuntu build::/src/a.cpp:3:5: error: use of undeclared identifier 'x'",
        ])

    def test_a_clean_log_gives_nothing(self):
        self.assertEqual(self.annotate("log", "Setting up weston (13.0.0)\n"), [])

    def test_at_most_nine_errors_and_one_more_line(self):
        text = "".join(f"E: problem {number}\n" for number in range(20))
        lines = self.annotate("log", text, title="t")
        self.assertEqual(len(lines), 10)
        self.assertEqual(lines[-1], "::error title=t::and 11 more error lines; see the job log")


class Files(Run):
    def test_a_missing_file_is_one_error(self):
        stdout = io.StringIO()
        missing = os.path.join(self.temp.name, "absent.txt")
        with contextlib.redirect_stdout(stdout):
            status = annotate.main(["junit", missing, "--title", "t"])
        self.assertEqual(status, 0)
        lines = stdout.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith(f"::error title=t::cannot read {missing}: "), lines)

    def test_script_exit_status(self):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write("FAILED: x.o\n")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "log", self.path, "--title", "t"],
                                capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "::error title=t::FAILED: x.o\n")
        result = subprocess.run([sys.executable, "-B", SCRIPT, "other", self.path, "--title", "t"],
                                capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
