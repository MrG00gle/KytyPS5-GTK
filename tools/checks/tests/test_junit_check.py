"""Unit tests for tools/checks/junit_check.py (task 28.1, Requirements 16.4, 16.5, 16.6).

RealCtest runs the CTest of this build (KYTY_CTEST, set by the junit_check_tool CTest; else the
ctest on PATH) on a generated scratch project, so the expected lines below pin the JUnit format
that this CTest version writes. The other classes use hand-written JUnit files for the cases CTest
does not produce.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CHECKS_DIR, "junit_check.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import junit_check  # noqa: E402


def run_main(*arguments):
    """Run junit_check.main and return (status, stdout lines, stderr)."""
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = junit_check.main([str(argument) for argument in arguments])
    return status, stdout.getvalue().splitlines(), stderr.getvalue()


def tool(variable, name):
    """The program named by the environment variable, else the one on PATH."""
    program = os.environ.get(variable) or shutil.which(name)
    if not program:
        raise AssertionError(f"{variable} is not set and there is no {name} on PATH")
    return program


# One test per CTest outcome. The commands run this Python, so the project needs no compiler.
SCRATCH_PROJECT = textwrap.dedent("""\
    cmake_minimum_required(VERSION 3.22)
    project(junit_check_scratch NONE)
    enable_testing()
    set(py "${PYTHON}")
    add_test(NAME passes COMMAND "${py}" -c "pass")
    add_test(NAME fails COMMAND "${py}" -c "raise SystemExit(1)")
    add_test(NAME skips COMMAND "${py}" -c "raise SystemExit(77)")
    set_tests_properties(skips PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME skips_by_output COMMAND "${py}" -c "print('SKIP-ME')")
    set_tests_properties(skips_by_output PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP-ME")
    add_test(NAME disabled COMMAND "${py}" -c "pass")
    set_tests_properties(disabled PROPERTIES DISABLED TRUE)
    add_test(NAME missing_executable COMMAND "${CMAKE_BINARY_DIR}/no-such-program")
    add_test(NAME times_out COMMAND "${py}" -c "import time; time.sleep(30)")
    set_tests_properties(times_out PROPERTIES TIMEOUT 1)
    add_test(NAME will_fail COMMAND "${py}" -c "raise SystemExit(1)")
    set_tests_properties(will_fail PROPERTIES WILL_FAIL TRUE)
    add_test(NAME setup_fails COMMAND "${py}" -c "raise SystemExit(1)")
    set_tests_properties(setup_fails PROPERTIES FIXTURES_SETUP scratch_fixture)
    add_test(NAME needs_fixture COMMAND "${py}" -c "pass")
    set_tests_properties(needs_fixture PROPERTIES FIXTURES_REQUIRED scratch_fixture)
    add_test(NAME [=[a<b>&c]=] COMMAND "${py}" -c "raise SystemExit(1)")
    add_test(NAME [=[x&amp;y]=] COMMAND "${py}" -c "pass")
    """)
SCRATCH_TESTS = 12
SCRATCH_PASSED = 3  # passes, will_fail, and x&amp;y
SCRATCH_PROBLEMS = [
    "failed: a<b>&c",
    "failed: fails",
    "failed: setup_fails",
    "failed: times_out",
    "skipped: skips (SKIP_RETURN_CODE=77)",
    "skipped: skips_by_output (SKIP_REGULAR_EXPRESSION_MATCHED)",
    "not run: disabled (disabled)",
    "not run: missing_executable (Unable to find executable)",
    "not run: needs_fixture (Fixture dependency failed)",
]


class RealCtest(unittest.TestCase):
    """The JUnit and --show-only=json-v1 files of a real CTest run."""

    @classmethod
    def setUpClass(cls):
        cmake = tool("KYTY_CMAKE", "cmake")
        ctest = tool("KYTY_CTEST", "ctest")
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source, build = root / "source", root / "build"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(SCRATCH_PROJECT, encoding="utf-8")
        configure = [cmake, "-S", str(source), "-B", str(build), f"-DPYTHON={sys.executable}"]
        # The generator of the calling build tree, so the scratch project needs no other build tool.
        if os.environ.get("KYTY_CMAKE_GENERATOR"):
            configure += ["-G", os.environ["KYTY_CMAKE_GENERATOR"]]
        if os.environ.get("KYTY_MAKE_PROGRAM"):
            configure += [f"-DCMAKE_MAKE_PROGRAM={os.environ['KYTY_MAKE_PROGRAM']}"]
        # The scratch tests must not inherit a parent CTest's settings.
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("CTEST_")}
        environment["PYTHONDONTWRITEBYTECODE"] = "1"

        def run(command):
            return subprocess.run(command, capture_output=True, text=True, env=environment,
                                  stdin=subprocess.DEVNULL, check=False)

        result = run(configure)
        if result.returncode != 0:
            raise AssertionError(f"configure failed:\n{result.stdout}\n{result.stderr}")
        cls.junit = root / "full.xml"
        cls.filtered_junit = root / "filtered.xml"
        cls.registered = root / "tests.json"
        cls.full_run = run([ctest, "--test-dir", str(build), "--output-junit", str(cls.junit)])
        cls.filtered_run = run([ctest, "--test-dir", str(build), "-E", "^(passes|fails)$",
                                "--output-junit", str(cls.filtered_junit)])
        listing = run([ctest, "--test-dir", str(build), "--show-only=json-v1"])
        if listing.returncode != 0:
            raise AssertionError(f"ctest --show-only failed:\n{listing.stderr}")
        cls.registered.write_text(listing.stdout, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_ctest_wrote_the_junit_file(self):
        # ctest exits 8 because tests failed or did not run.
        self.assertEqual(self.full_run.returncode, 8, self.full_run.stdout)
        self.assertTrue(self.junit.is_file())
        self.assertTrue(self.filtered_junit.is_file())

    def test_every_outcome_is_listed(self):
        status, lines, stderr = run_main(self.junit)
        self.assertEqual(status, 1, stderr)
        self.assertEqual(lines[:-1], SCRATCH_PROBLEMS)
        self.assertEqual(lines[-1], f"junit-check: {SCRATCH_TESTS} tests, {SCRATCH_PASSED} "
                         f"passed, {len(SCRATCH_PROBLEMS)} problems")

    def test_registered_tests_of_a_full_run_are_all_present(self):
        status, lines, _ = run_main(self.junit, "--registered", self.registered)
        self.assertEqual(status, 1)
        self.assertEqual(lines[:-1], SCRATCH_PROBLEMS)
        listed = json.loads(self.registered.read_text(encoding="utf-8"))["tests"]
        self.assertEqual(len(listed), SCRATCH_TESTS)

    def test_a_filtered_run_lacks_the_excluded_tests(self):
        status, lines, _ = run_main(self.filtered_junit, "--registered", self.registered)
        self.assertEqual(status, 1)
        self.assertEqual(lines[-3:-1], ["missing: fails", "missing: passes"])
        self.assertNotIn("failed: fails", lines)
        self.assertEqual(lines[-1], f"junit-check: {SCRATCH_TESTS - 2} tests, "
                         f"{SCRATCH_PASSED - 1} passed, {len(SCRATCH_PROBLEMS) + 1} problems")

    def test_a_filtered_run_is_not_detected_without_registered(self):
        _, lines, _ = run_main(self.filtered_junit)
        self.assertFalse([line for line in lines if line.startswith("missing")])


class JunitFiles(unittest.TestCase):
    """Hand-written JUnit and test list files in a temporary directory."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def junit(self, body, root="testsuite", name="ctest.xml"):
        path = self.root / name
        path.write_text(f'<?xml version="1.0" encoding="UTF-8"?>\n<{root}>\n{body}\n</{root}>\n',
                        encoding="utf-8")
        return path

    def registered(self, names, text=None):
        path = self.root / "tests.json"
        document = {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
                    "tests": [{"name": name} for name in names]}
        path.write_text(text if text is not None else json.dumps(document), encoding="utf-8")
        return path


def case(name, status="run", child=""):
    return f'<testcase name="{name}" classname="{name}" time="0.1" status="{status}">{child}' \
           '<properties/><system-out></system-out></testcase>'


class Outcomes(JunitFiles):
    def test_all_passing_exits_zero(self):
        path = self.junit("\n".join(case(name) for name in ("b", "a", "c")))
        status, lines, _ = run_main(path)
        self.assertEqual(status, 0)
        self.assertEqual(lines, ["junit-check: 3 tests, 3 passed, 0 problems"])

    def test_problems_are_grouped_by_kind_then_sorted_by_name(self):
        path = self.junit("\n".join([
            case("z_skip", "notrun", '<skipped message="SKIP_RETURN_CODE=77"/>'),
            case("y_fail", "fail", '<failure message="Failed"/>'),
            case("b_notrun", "notrun", '<skipped message="Unable to find executable"/>'),
            case("a_fail", "fail", '<failure message="Timeout"/>'),
            case("ok"),
        ]))
        status, lines, _ = run_main(path)
        self.assertEqual(status, 1)
        self.assertEqual(lines, [
            "failed: a_fail",
            "failed: y_fail",
            "skipped: z_skip (SKIP_RETURN_CODE=77)",
            "not run: b_notrun (Unable to find executable)",
            "junit-check: 5 tests, 1 passed, 4 problems",
        ])

    def test_a_failure_or_error_child_fails_whatever_the_status(self):
        path = self.junit("\n".join([case("with_error", child='<error message="crash"/>'),
                                     case("with_failure", child='<failure/>')]))
        _, lines, _ = run_main(path)
        self.assertEqual(lines[:2], ["failed: with_error", "failed: with_failure"])

    def test_a_skipped_child_of_a_run_test_is_a_skip(self):
        path = self.junit("\n".join([case("plain", child="<skipped/>"),
                                     case("told", child='<skipped message="no\n  display"/>')]))
        status, lines, _ = run_main(path)
        self.assertEqual(status, 1)
        self.assertEqual(lines[:2], ["skipped: plain (no message)", "skipped: told (no display)"])

    def test_unknown_or_missing_status_is_not_run(self):
        path = self.junit("\n".join([case("odd", "weird"),
                                     '<testcase name="bare"/>',
                                     case("off", "disabled")]))
        status, lines, _ = run_main(path)
        self.assertEqual(status, 1)
        self.assertEqual(lines[:3], ["not run: bare (no status)", "not run: odd (weird)",
                                     "not run: off (disabled)"])

    def test_names_with_xml_entities_are_printed_decoded(self):
        path = self.junit(case("a&amp;b &lt;c&gt; &quot;d&quot;", "fail", "<failure/>"))
        _, lines, _ = run_main(path)
        self.assertEqual(lines[0], 'failed: a&b <c> "d"')

    def test_testsuites_root_counts_every_suite(self):
        body = "<testsuite>" + case("one") + case("two", "fail", "<failure/>") + \
               "</testsuite>\n<testsuite>" + case("three") + "</testsuite>"
        status, lines, _ = run_main(self.junit(body, root="testsuites"))
        self.assertEqual(status, 1)
        self.assertEqual(lines, ["failed: two", "junit-check: 3 tests, 2 passed, 1 problems"])

    def test_duplicate_names_are_each_counted(self):
        path = self.junit(case("again") + case("again", "fail", "<failure/>"))
        _, lines, _ = run_main(path)
        self.assertEqual(lines, ["failed: again", "junit-check: 2 tests, 1 passed, 1 problems"])


class Registered(JunitFiles):
    def test_missing_names_are_listed_after_the_other_problems(self):
        path = self.junit(case("present") + case("broken", "fail", "<failure/>"))
        tests = self.registered(["zeta", "present", "broken", "alpha"])
        status, lines, _ = run_main(path, "--registered", tests)
        self.assertEqual(status, 1)
        self.assertEqual(lines, ["failed: broken", "missing: alpha", "missing: zeta",
                                 "junit-check: 2 tests, 1 passed, 3 problems"])

    def test_every_registered_name_present_passes(self):
        path = self.junit(case("one") + case("two"))
        status, lines, _ = run_main(path, "--registered", self.registered(["two", "one"]))
        self.assertEqual(status, 0)
        self.assertEqual(lines, ["junit-check: 2 tests, 2 passed, 0 problems"])

    def test_tests_outside_the_list_are_not_reported(self):
        path = self.junit(case("one") + case("extra"))
        status, _, _ = run_main(path, "--registered", self.registered(["one"]))
        self.assertEqual(status, 0)

    def test_bad_test_lists_are_input_errors(self):
        path = self.junit(case("one"))
        for text, message in (("not json", "is not JSON"), ("[]", 'has no "tests" list'),
                              ('{"tests": {}}', 'has no "tests" list'),
                              ('{"tests": [{"name": ""}]}', "a test has no name"),
                              ('{"tests": [{}]}', "a test has no name")):
            with self.subTest(text=text):
                status, lines, stderr = run_main(path, "--registered", self.registered([], text))
                self.assertEqual(status, 2)
                self.assertEqual(lines, [])
                self.assertIn(message, stderr)

    def test_absent_test_list_is_an_input_error(self):
        status, _, stderr = run_main(self.junit(case("one")), "--registered", self.root / "none")
        self.assertEqual(status, 2)
        self.assertIn("cannot read", stderr)


class BadJunit(JunitFiles):
    def test_zero_testcases_fails(self):
        for body, root in (("", "testsuite"), ("<testsuite/>", "testsuites")):
            with self.subTest(root=root):
                path = self.junit(body, root=root)
                status, lines, _ = run_main(path)
                self.assertEqual(status, 1)
                self.assertEqual(lines, [f"no tests: {path} has no testcase",
                                         "junit-check: 0 tests, 0 passed, 1 problems"])

    def test_zero_testcases_with_registered_lists_every_test_too(self):
        path = self.junit("")
        status, lines, _ = run_main(path, "--registered", self.registered(["one"]))
        self.assertEqual(status, 1)
        self.assertEqual(lines[0], "missing: one")
        self.assertEqual(lines[-1], "junit-check: 0 tests, 0 passed, 2 problems")

    def test_malformed_xml_is_an_input_error(self):
        path = self.root / "broken.xml"
        path.write_text('<testsuite><testcase name="a" status="run"></testsuite>', encoding="utf-8")
        status, lines, stderr = run_main(path)
        self.assertEqual(status, 2)
        self.assertEqual(lines, [])
        self.assertIn("is not well-formed XML", stderr)

    def test_other_root_element_is_an_input_error(self):
        status, _, stderr = run_main(self.junit(case("a"), root="report"))
        self.assertEqual(status, 2)
        self.assertIn("the root element is <report>", stderr)

    def test_unnamed_testcase_is_an_input_error(self):
        status, _, stderr = run_main(self.junit('<testcase status="run"/>'))
        self.assertEqual(status, 2)
        self.assertIn("a testcase has no name", stderr)

    def test_absent_file_is_an_input_error(self):
        status, _, stderr = run_main(self.root / "absent.xml")
        self.assertEqual(status, 2)
        self.assertIn("cannot read", stderr)


class CommandLine(JunitFiles):
    def run_script(self, *arguments):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        return subprocess.run([sys.executable, "-B", SCRIPT, *map(str, arguments)],
                              capture_output=True, text=True, env=environment,
                              stdin=subprocess.DEVNULL, check=False)

    def test_script_exit_status_and_output(self):
        path = self.junit(case("one") +
                          case("two", "notrun", '<skipped message="SKIP_RETURN_CODE=77"/>'))
        result = self.run_script(path)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["skipped: two (SKIP_RETURN_CODE=77)",
                          "junit-check: 2 tests, 1 passed, 1 problems"])

    def test_script_requires_the_junit_file(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 2)
        self.assertIn("junit", result.stderr)


if __name__ == "__main__":
    unittest.main()
