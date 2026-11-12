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
    unexpected pass: <name> (...)
    bad rerun: <name> (...)

then "junit-check: <n> tests, <p> passed, <k> problems". With --registered,
every test that `ctest --show-only=json-v1` lists must appear in the JUnit
file; a registered test without a testcase was not run ("missing"), for
example because the run was filtered. The JUnit root may be <testsuite> (what
CTest writes) or <testsuites>.

Two CI policies that the repository owner approved for named tests only
(docs/testing.md); no other test is ever accepted by them:

--expected-failures FILE (tools/checks/ci-expected-failures.txt): GPU tests
    that fail on CI's software Vulkan driver because it lacks a feature. Each
    line is "<test name> <exact failure text>". A listed test passes the check
    only when it failed and its output contains that text ("expected failure:
    <name>"). It is a problem when it failed without the text ("failed"), when
    it passed ("unexpected pass": remove it from the list), when it was skipped
    or not run, and when the JUnit file has no testcase for it ("missing").
--retry-list FILE (tools/checks/ci-timing-retries.txt): timing tests that a
    noisy CI machine may push over their limit. One test name per line. A
    listed test that failed in this (full, unfiltered) run is accepted only
    when the JUnit file of the rerun, --rerun RERUN_JUNIT, has a passing
    testcase for it ("retried: <name> (...)", with the worst TIMING median of
    both runs when the output has TIMING lines). Without --rerun it stays a
    failure. The rerun may only hold listed tests that failed here ("bad
    rerun" otherwise). A skipped or not-run listed test is never retried.
    With --print-retry-candidates the script only prints the listed tests that
    failed here, one per line, for the rerun step (exit 0, or 2 on an input
    error).
With either option the summary line also counts the expected failures and the
retried tests. The two lists may not share a test.

Usage:
    junit_check.py JUNIT [--registered TESTS_JSON] [--expected-failures FILE]
                   [--retry-list FILE [--rerun RERUN_JUNIT | --print-retry-candidates]]

Exit status: 0 when every testcase passed or was accepted by a policy (and,
with --registered, every registered test has one), 1 when a test failed, was
skipped, did not run, or is missing, or when the file has no testcase at all,
2 on a usage or input error (an unreadable or malformed file, another root
element, a test without a name, or a malformed or empty list file).
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET

# The <skipped> messages of tests that CTest started and then counted as skipped.
SKIP_MESSAGE = re.compile(r"SKIP_RETURN_CODE=\d+|SKIP_REGULAR_EXPRESSION_MATCHED")
ROOTS = ("testsuite", "testsuites")
KINDS = ("failed", "skipped", "not run", "missing", "unexpected pass", "bad rerun")
# A listed name: also used verbatim in the rerun step's `ctest -R '^(<names>)$'`.
LIST_NAME = re.compile(r"[A-Za-z0-9_]+")
# "TIMING <case> <backend> <view> "<query>" shown=<n> min=<ms> median=<ms> ..." (tests/ui).
TIMING_MEDIAN = re.compile(r"^TIMING .* median=([0-9]+(?:\.[0-9]+)?) ", re.M)


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


def read_list_lines(path):
    """Return (line number, text) of the lines of a list file that are not blank or # comments."""
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    entries = [(number, line.strip()) for number, line in enumerate(lines, 1)
               if line.strip() and not line.strip().startswith("#")]
    if not entries:
        raise InputError(f"{path} lists no tests")
    return entries


def checked_name(path, number, name, seen):
    if not LIST_NAME.fullmatch(name):
        raise InputError(f"{path}:{number}: {name!r} is not a test name (letters, digits, and _)")
    if name in seen:
        raise InputError(f"{path}:{number}: {name} is listed twice")
    return name


def read_expected_failures(path):
    """Return {test name: exact failure text} of a "<name> <text>" list."""
    expected = {}
    for number, line in read_list_lines(path):
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise InputError(f"{path}:{number}: not a '<test name> <failure text>' line")
        expected[checked_name(path, number, parts[0], expected)] = parts[1].strip()
    return expected


def read_retry_list(path):
    """Return the test names of a one-name-per-line list."""
    names = []
    for number, line in read_list_lines(path):
        names.append(checked_name(path, number, line, names))
    return names


def output_of(case):
    """The output CTest recorded for a testcase, with the failure message and text."""
    parts = [element.text or "" for element in case.iter("system-out")]
    for child in case:
        if child.tag in ("failure", "error"):
            parts.extend((child.get("message") or "", child.text or ""))
    return "\n".join(parts)


def worst_median(case):
    """The largest TIMING median of a testcase's output, or None without TIMING lines."""
    medians = [float(value) for value in TIMING_MEDIAN.findall(output_of(case))]
    return max(medians) if medians else None


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


def retry_candidates(cases, retry):
    """The listed tests that failed in this run, sorted."""
    listed = set(retry)
    return sorted({case.get("name") for case in cases if case.get("name") in listed
                   and (classify(case) or ("",))[0] == "failed"})


def run_text(label, case):
    median = worst_median(case)
    return f"{label}, worst median {median:.1f} ms" if median is not None else label


def check(cases, registered=None, expected=None, retry=None, rerun=None):
    """Return (accepted lines, problem lines, number of passing testcases).

    expected: {name: failure text} or None. retry: listed names or None. rerun: the rerun's
    testcases or None.
    """
    expected = expected or {}
    retry_set = set(retry or ())
    problems = {kind: [] for kind in KINDS}
    accepted = []
    passed = 0
    rerun_by_name = {case.get("name"): case for case in rerun or ()}
    full_failures = set()
    for case in cases:
        name = case.get("name")
        result = classify(case)
        if name in expected:
            if result is None:
                problems["unexpected pass"].append(
                    (name, "listed as an expected failure; remove it from the list"))
            elif result[0] == "failed" and expected[name] in output_of(case):
                accepted.append(f"expected failure: {name}")
            elif result[0] == "failed":
                problems["failed"].append((name, "its output lacks the listed failure text"))
            else:
                problems[result[0]].append((name, result[1]))
            continue
        if result is None:
            passed += 1
            continue
        kind, detail = result
        if name in retry_set and kind == "failed":
            full_failures.add(name)
            first = run_text("failed in the full run", case)
            again = rerun_by_name.get(name)
            if rerun is None:
                problems["failed"].append((name, f"listed for a rerun; {first}; no rerun results"))
            elif again is None:
                problems["failed"].append((name, f"listed for a rerun; {first}; not rerun"))
            elif classify(again) is None:
                second = run_text("passed in the rerun", again)
                accepted.append(f"retried: {name} ({first}; {second})")
            else:
                second = run_text("failed in the rerun too", again)
                problems["failed"].append((name, f"listed for a rerun; {first}; {second}"))
            continue
        problems[kind].append((name, detail))
    present = {case.get("name") for case in cases}
    if registered is not None:
        problems["missing"] = [(name, None) for name in set(registered) - present]
    for name in sorted(set(expected) - present):
        problems["missing"].append((name, "listed as an expected failure"))
    for name in rerun_by_name:
        if name not in retry_set:
            problems["bad rerun"].append((name, "the rerun ran a test that the retry list lacks"))
        elif name not in full_failures:
            problems["bad rerun"].append((name, "the rerun ran a test that did not fail in the "
                                          "full run"))
    lines = []
    for kind in KINDS:
        for name, detail in sorted(problems[kind]):
            lines.append(f"{kind}: {name}" + (f" ({detail})" if detail else ""))
    return sorted(accepted), lines, passed


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="List every failed, skipped, or not-run test of a CTest JUnit file "
        "(and, with --registered, every registered test that it lacks); exit 1 if there is one.")
    parser.add_argument("junit", help="the file that ctest --output-junit wrote")
    parser.add_argument("--registered", metavar="TESTS_JSON",
                        help="ctest --show-only=json-v1 output for the same build tree: every "
                        "test it lists must appear in the JUnit file")
    parser.add_argument("--expected-failures", metavar="FILE",
                        help="'<test name> <exact failure text>' lines: those tests must fail "
                        "with that text")
    parser.add_argument("--retry-list", metavar="FILE",
                        help="one test name per line: those tests may pass in a rerun instead")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--rerun", metavar="RERUN_JUNIT",
                      help="the JUnit file of the rerun of the listed tests that failed")
    mode.add_argument("--print-retry-candidates", action="store_true",
                      help="only print the listed tests that failed, one per line")
    args = parser.parse_args(argv)
    if (args.rerun is not None or args.print_retry_candidates) and args.retry_list is None:
        parser.error("--rerun and --print-retry-candidates need --retry-list")

    try:
        cases = read_testcases(args.junit)
        registered = read_registered(args.registered) if args.registered is not None else None
        expected = (read_expected_failures(args.expected_failures)
                    if args.expected_failures is not None else None)
        retry = read_retry_list(args.retry_list) if args.retry_list is not None else None
        if expected and retry:
            shared = sorted(set(expected) & set(retry))
            if shared:
                raise InputError(f"{', '.join(shared)} is in both {args.expected_failures} and "
                                 f"{args.retry_list}")
        rerun = read_testcases(args.rerun) if args.rerun is not None else None
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.print_retry_candidates:
        for name in retry_candidates(cases, retry):
            print(name)
        return 0

    accepted, lines, passed = check(cases, registered, expected, retry, rerun)
    if not cases:
        lines.append(f"no tests: {args.junit} has no testcase")
    for line in accepted + lines:
        print(line)
    if expected is None and retry is None:
        print(f"junit-check: {len(cases)} tests, {passed} passed, {len(lines)} problems")
    else:
        failures = sum(line.startswith("expected failure: ") for line in accepted)
        retried = sum(line.startswith("retried: ") for line in accepted)
        print(f"junit-check: {len(cases)} tests, {passed} passed, {failures} expected failures, "
              f"{retried} retried, {len(lines)} problems")
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main())
