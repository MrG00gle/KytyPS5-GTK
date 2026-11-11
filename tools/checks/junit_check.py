#!/usr/bin/env python3
"""Check a CTest JUnit file: every test must have run and passed.

Requirements 16.4, 16.5, and 16.6: CI runs every registered test, and a test
that failed, was skipped (a GPU test that exits with the skip code, a UI test
without a display), or did not run counts as a failure. CTest's own exit status
does not show skipped tests, so CI runs this script on the file that
`ctest --output-junit` wrote.

A testcase passes only with status="run" and no <failure>, <error>, or
<skipped> child. CTest writes status="notrun" with a <skipped> child both for a
skipped test (message "SKIP_RETURN_CODE=<n>" or
"SKIP_REGULAR_EXPRESSION_MATCHED") and for a test it could not start (for
example "Unable to find executable" or "Fixture dependency failed"), and
status="disabled" for a disabled test. This script names them the way CTest's
own summary does. It prints one line per problem, grouped in this order and
sorted by test name within each group:

    failed: <name>
    skipped: <name> (<message>)
    not run: <name> (<message, or the status attribute>)
    missing: <name>

then "junit-check: <n> tests, <p> passed, <k> problems". With --registered,
every test that `ctest --show-only=json-v1` lists must appear in the JUnit
file; a registered test without a testcase was not run ("missing"), for
example because the run was filtered. The JUnit root may be <testsuite> (what
CTest writes) or <testsuites>.

Usage:
    junit_check.py JUNIT [--registered TESTS_JSON]

Exit status: 0 when every testcase passed (and, with --registered, every
registered test has one), 1 when a test failed, was skipped, did not run, or
is missing, or when the file has no testcase at all, 2 on a usage or input
error (an unreadable or malformed file, another root element, or a test
without a name).
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET

# The <skipped> messages of tests that CTest started and then counted as skipped.
SKIP_MESSAGE = re.compile(r"SKIP_RETURN_CODE=\d+|SKIP_REGULAR_EXPRESSION_MATCHED")
ROOTS = ("testsuite", "testsuites")
KINDS = ("failed", "skipped", "not run", "missing")


class InputError(Exception):
    """An input file cannot be used."""


def one_line(text):
    """Collapse the whitespace of an XML attribute or message into single spaces."""
    return " ".join(text.split())


def read_testcases(path):
    """Return the <testcase> elements of a JUnit file."""
    try:
        root = ET.parse(path).getroot()
    except OSError as error:
        raise InputError(f"cannot read {path}: {error}") from error
    except ET.ParseError as error:
        raise InputError(f"{path} is not well-formed XML: {error}") from error
    if root.tag not in ROOTS:
        raise InputError(f"{path}: the root element is <{root.tag}>, not <testsuite> or "
                         "<testsuites>")
    cases = list(root.iter("testcase"))
    for case in cases:
        if not case.get("name"):
            raise InputError(f"{path}: a testcase has no name")
    return cases


def read_registered(path):
    """Return the test names of `ctest --show-only=json-v1` output."""
    try:
        with open(path, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise InputError(f"{path} is not JSON: {error}") from error
    tests = document.get("tests") if isinstance(document, dict) else None
    if not isinstance(tests, list):
        raise InputError(f"{path} has no \"tests\" list (expected ctest --show-only=json-v1 "
                         "output)")
    names = []
    for test in tests:
        name = test.get("name") if isinstance(test, dict) else None
        if not isinstance(name, str) or not name:
            raise InputError(f"{path}: a test has no name")
        names.append(name)
    return names


def classify(case):
    """Return None for a passing testcase, else (kind, detail or None)."""
    status = case.get("status")
    if status == "fail" or case.find("failure") is not None or case.find("error") is not None:
        return ("failed", None)
    skipped = case.find("skipped")
    if status == "run" and skipped is None:
        return None
    message = one_line(skipped.get("message") or "") if skipped is not None else ""
    if skipped is not None and (status != "notrun" or SKIP_MESSAGE.fullmatch(message)):
        return ("skipped", message or "no message")
    return ("not run", message or status or "no status")


def check(cases, registered=None):
    """Return (problem lines in output order, number of passing testcases)."""
    problems = {kind: [] for kind in KINDS}
    passed = 0
    for case in cases:
        result = classify(case)
        if result is None:
            passed += 1
            continue
        kind, detail = result
        problems[kind].append((case.get("name"), detail))
    if registered is not None:
        present = {case.get("name") for case in cases}
        problems["missing"] = [(name, None) for name in set(registered) - present]
    lines = []
    for kind in KINDS:
        for name, detail in sorted(problems[kind]):
            lines.append(f"{kind}: {name}" + (f" ({detail})" if detail else ""))
    return lines, passed


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="List every failed, skipped, or not-run test of a CTest JUnit file "
        "(and, with --registered, every registered test that it lacks); exit 1 if there is one.")
    parser.add_argument("junit", help="the file that ctest --output-junit wrote")
    parser.add_argument("--registered", metavar="TESTS_JSON",
                        help="ctest --show-only=json-v1 output for the same build tree: every "
                        "test it lists must appear in the JUnit file")
    args = parser.parse_args(argv)

    try:
        cases = read_testcases(args.junit)
        registered = read_registered(args.registered) if args.registered is not None else None
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    lines, passed = check(cases, registered)
    if not cases:
        lines.append(f"no tests: {args.junit} has no testcase")
    for line in lines:
        print(line)
    print(f"junit-check: {len(cases)} tests, {passed} passed, {len(lines)} problems")
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main())
