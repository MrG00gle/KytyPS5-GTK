#!/usr/bin/env python3
"""Turn the output of a CI check into GitHub Actions annotations.

The workflow's steps write their check output to a file and call this script on it when the step
fails (and, for the coverage check and the job's environment, always). The annotations appear on
the run page and through the check-runs API, so a failed run can be read without its job logs.

Usage:
    annotate.py junit FILE --title TITLE
    annotate.py coverage FILE --title TITLE
    annotate.py log FILE --title TITLE
    annotate.py environment FILE --title TITLE [--summary PATH]

junit: FILE is the output of tools/checks/junit_check.py. Each problem line ("failed:",
    "skipped:", "not run:", "missing:", "unexpected pass:", "bad rerun:", "no tests:") becomes an
    error, each "retried:" line a warning, and the "junit-check:" summary line a notice.
coverage: FILE is the output of tools/coverage/check.py. Each "FAIL" line becomes an error, and
    one notice gives the measured scope line coverage and the line coverage of every
    Requirement 18.4 directory.
log: FILE is a command's log (a package install or a build). The first lines that start with
    "E: " (apt), "Error:" (dnf), or "FAILED:" (ninja), or that contain "error:" (compilers,
    CMake, ninja), become errors.
environment: FILE has one "label<TAB>value" line per item of the job's environment (the OS, the
    kernel, package versions); blank lines are ignored, and a line without a TAB is a label with
    an empty value. Each item is printed as "label: value" for the job log, and one notice titled
    "TITLE environment" lists them all. A "### TITLE environment" heading and a two-column table
    of the items (with | escaped in the cells) are appended to PATH: --summary, else
    $GITHUB_STEP_SUMMARY when it is set and not empty, else nothing is appended. A FILE without
    an item, or a PATH that cannot be written, is an error. --summary is for this mode only.

Each step gets at most 9 errors (and 9 warnings) plus one "and N more" line: GitHub shows only 10
error annotations of a step. A missing or unreadable FILE is one error, and so is a junit or
coverage FILE without the check's result (the check stopped on an input error; the job log has
its message). Messages and the title are escaped as workflow commands require.

Exit status: 0 when the annotations were printed (whatever they say), 2 on a usage error.
"""

import argparse
import os
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


def environment_items(text):
    """(label, value) of each non-blank "label<TAB>value" line."""
    items = []
    for line in text.splitlines():
        if line.strip():
            label, _, value = line.partition("\t")
            items.append((label.strip(), value.strip()))
    return items


def table_cell(text):
    return text.replace("|", "\\|")


def environment_annotations(text, title, summary=None):
    """The log lines and the notice for the items of text; the table is appended to summary (a
    path, or None for no summary)."""
    items = environment_items(text)
    if not items:
        return [command("error", title, "the environment file lists no items")]
    lines = [f"{label}: {value}".rstrip() for label, value in items]
    if summary:
        table = [f"### {title} environment", "", "| Item | Value |", "| --- | --- |"]
        table += [f"| {table_cell(label)} | {table_cell(value)} |" for label, value in items]
        try:
            with open(summary, "a", encoding="utf-8") as handle:
                handle.write("\n".join(table) + "\n\n")
        except OSError as error:
            lines.append(command("error", title, f"cannot write the job summary {summary}: "
                                 f"{error.strerror}"))
    message = "; ".join(f"{label} {value}".rstrip() for label, value in items)
    lines.append(command("notice", f"{title} environment", message))
    return lines


MODES = {"junit": junit_annotations, "coverage": coverage_annotations, "log": log_annotations,
         "environment": environment_annotations}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Print GitHub Actions annotations for the output of a CI check.")
    parser.add_argument("mode", choices=sorted(MODES), help="what kind of output FILE holds")
    parser.add_argument("file", help="the output file")
    parser.add_argument("--title", required=True, help="the annotations' title")
    parser.add_argument("--summary", help="environment mode: the job summary file to append the "
                        "table to (default: $GITHUB_STEP_SUMMARY)")
    args = parser.parse_args(argv)
    if args.summary is not None and args.mode != "environment":
        parser.error("--summary is only for the environment mode")
    try:
        with open(args.file, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        print(command("error", args.title, f"cannot read {args.file}: {error.strerror}"))
        return 0
    if args.mode == "environment":
        summary = args.summary if args.summary is not None else os.environ.get(
            "GITHUB_STEP_SUMMARY", "")
        lines = environment_annotations(text, args.title, summary)
    else:
        lines = MODES[args.mode](text, args.title)
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
