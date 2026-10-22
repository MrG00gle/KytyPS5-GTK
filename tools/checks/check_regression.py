#!/usr/bin/env python3
"""Check a CTest JUnit report against the Main_Branch passing-test baseline.

Requirement 5.4: every test that CTest reported as passed at the head of the
Main_Branch (listed in main-passing-tests.txt) must be reported as passed,
under the same CTest name, by the current build.

Usage:
    check_regression.py [--baseline FILE] JUNIT_XML

JUNIT_XML is the file written by `ctest --output-junit <file>`. A relative
--output-junit path is resolved by CTest against the build directory.

Exit status: 0 when every baseline test passed, 1 when at least one baseline
test is missing or did not pass (each one is listed), 2 on a usage or input
error.
"""

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

DEFAULT_BASELINE = Path(__file__).resolve().parent / "main-passing-tests.txt"

# CTest writes status="run" for a completed (passed) test, "fail" for a
# failure, "notrun" for a skipped or not-run test, and "disabled" for a
# disabled test.
PASSED_STATUSES = {"run", "passed", "pass"}
NON_PASS_CHILDREN = ("failure", "error", "skipped")


def read_baseline(path):
    """Return the test names in the baseline file, in file order.

    Lines starting with '#' are header or comment lines. Blank lines are
    ignored. Every other line is one CTest test name.
    """
    names = []
    seen = set()
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            name = raw.strip()
            if not name or name.startswith("#"):
                continue
            if name not in seen:
                seen.add(name)
                names.append(name)
    return names


def testcase_outcome(testcase):
    """Return None when the testcase passed, else a short reason string."""
    for tag in NON_PASS_CHILDREN:
        child = testcase.find(tag)
        if child is not None:
            message = child.get("message", "").strip()
            status = testcase.get("status", tag)
            return f"{status}: {message}" if message else status
    status = testcase.get("status")
    if status is not None and status.strip().lower() not in PASSED_STATUSES:
        return status
    return None


def read_junit(path):
    """Map each testcase name to a list of outcomes (None means passed)."""
    root = ET.parse(path).getroot()
    outcomes = {}
    for testcase in root.iter("testcase"):
        name = testcase.get("name")
        if name is None:
            continue
        outcomes.setdefault(name, []).append(testcase_outcome(testcase))
    return outcomes


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Fail when a Main_Branch passing test is not reported as "
        "passed under the same name in a CTest JUnit file.")
    parser.add_argument("junit", type=Path,
                        help="JUnit XML file written by ctest --output-junit")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE,
                        help="baseline test list (default: %(default)s)")
    args = parser.parse_args(argv)

    try:
        baseline = read_baseline(args.baseline)
    except OSError as error:
        print(f"error: cannot read baseline {args.baseline}: {error}",
              file=sys.stderr)
        return 2
    if not baseline:
        print(f"error: baseline {args.baseline} lists no tests",
              file=sys.stderr)
        return 2

    try:
        outcomes = read_junit(args.junit)
    except (OSError, ET.ParseError) as error:
        print(f"error: cannot read JUnit file {args.junit}: {error}",
              file=sys.stderr)
        return 2

    regressions = []
    for name in baseline:
        results = outcomes.get(name)
        if not results:
            regressions.append((name, "not reported"))
            continue
        failures = [reason for reason in results if reason is not None]
        if failures:
            regressions.append((name, failures[0]))

    if regressions:
        print(f"{len(regressions)} of {len(baseline)} baseline tests were not "
              "reported as passed:")
        for name, reason in regressions:
            print(f"  {name} ({reason})")
        return 1

    print(f"All {len(baseline)} baseline tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
