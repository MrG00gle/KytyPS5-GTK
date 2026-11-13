#!/usr/bin/env python3
"""Turn the output of a CI check into GitHub Actions annotations.

The workflow's steps write their check output to a file and call this script on it when the step
fails (and, for the coverage check, always). The annotations appear on the run page and through
the check-runs API, so a failed run can be read without its job logs.

Usage:
    annotate.py junit FILE --title TITLE
    annotate.py coverage FILE --title TITLE
    annotate.py log FILE --title TITLE

junit: FILE is the output of tools/checks/junit_check.py. Each problem line ("failed:",
    "skipped:", "not run:", "missing:", "unexpected pass:", "bad rerun:", "no tests:") becomes an
    error, each "retried:" line a warning, and the "junit-check:" summary line a notice.
coverage: FILE is the output of tools/coverage/check.py. Each "FAIL" line becomes an error, and
    one notice gives the measured scope line coverage and the line coverage of every
    Requirement 18.4 directory.
log: FILE is a command's log (a package install or a build). The first lines that start with
    "E: " (apt), "Error:" (dnf), or "FAILED:" (ninja), or that contain "error:" (compilers,
    CMake, ninja), become errors.

Each step gets at most 9 errors (and 9 warnings) plus one "and N more" line: GitHub shows only 10
error annotations of a step. A missing or unreadable FILE is one error, and so is a junit or
coverage FILE without the check's result (the check stopped on an input error; the job log has
its message). Messages and the title are escaped as workflow commands require.

Exit status: 0 when the annotations were printed (whatever they say), 2 on a usage error.
"""

import argparse
import re
import sys

LIMIT = 9
JUNIT_ERRORS = ("failed:", "skipped:", "not run:", "missing:", "unexpected pass:", "bad rerun:",
                "no tests:")
JUNIT_WARNING = "retried:"
JUNIT_SUMMARY = "junit-check:"
SCOPE_LINE = re.compile(r"^scope line coverage: ([0-9]+\.[0-9]{2})%")
DIRECTORY_LINE = re.compile(r"^(?:PASS|FAIL) directory (\S+) lines: measured ([0-9]+\.[0-9]{2})%")
LOG_ERROR = re.compile(r"^\s*(?:E: |Error:|FAILED:)|(?<![\w-])error:")


def escape_data(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property(text):
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def command(kind, title, message):
    return f"::{kind} title={escape_property(title)}::{escape_data(message)}"


def capped(kind, title, messages, noun):
    """The workflow commands for messages, at most LIMIT and one line for the rest."""
    lines = [command(kind, title, message) for message in messages[:LIMIT]]
    rest = len(messages) - LIMIT
    if rest > 0:
        lines.append(command(kind, title, f"and {rest} more {noun}; see the job log"))
    return lines


def junit_annotations(text, title):
    errors, warnings, notices = [], [], []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith(JUNIT_ERRORS):
            errors.append(line)
        elif line.startswith(JUNIT_WARNING):
            warnings.append(line)
        elif line.startswith(JUNIT_SUMMARY):
            notices.append(line)
    if not notices:
        errors.append("junit_check.py printed no junit-check: summary; see the job log")
    return (capped("error", title, errors, "problems") + capped("warning", title, warnings,
            "retried tests") + [command("notice", title, line) for line in notices])


def coverage_annotations(text, title):
    errors = []
    scope = None
    directories = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("FAIL "):
            errors.append(line[len("FAIL "):])
        match = SCOPE_LINE.match(line)
        if match and scope is None:
            scope = match.group(1)
        match = DIRECTORY_LINE.match(line)
        if match:
            directories.append(f"{match.group(1)} {match.group(2)}%")
    if scope is None and not errors:
        errors.append("check.py printed no result; see the job log")
    lines = capped("error", title, errors, "failures")
    if scope is not None:
        message = f"scope line coverage {scope}%"
        if directories:
            message += "; directory lines: " + ", ".join(directories)
        lines.append(command("notice", title, message))
    return lines


def log_annotations(text, title):
    errors = [line.strip() for line in text.splitlines() if LOG_ERROR.search(line)]
    return capped("error", title, errors, "error lines")


MODES = {"junit": junit_annotations, "coverage": coverage_annotations, "log": log_annotations}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Print GitHub Actions annotations for the output of a CI check.")
    parser.add_argument("mode", choices=sorted(MODES), help="what kind of output FILE holds")
    parser.add_argument("file", help="the output file")
    parser.add_argument("--title", required=True, help="the annotations' title")
    args = parser.parse_args(argv)
    try:
        with open(args.file, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        print(command("error", args.title, f"cannot read {args.file}: {error.strerror}"))
        return 0
    for line in MODES[args.mode](text, args.title):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
