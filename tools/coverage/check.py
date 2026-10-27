#!/usr/bin/env python3
"""Enforce the coverage targets of Requirement 18 on a coverage summary.

Rules:
- the summary written by report.py must exist and be valid (18.10);
- every group threshold in groups.json must be met (18.1 to 18.3, 18.9);
- every Requirement 18.4 directory must have covered lines (18.4, 18.9);
- the baseline file must hold one line matching ^\\d{1,3}\\.\\d{2}$ with a
  value from 0.00 to 100.00; a missing or malformed file fails and prints
  the measured scope line coverage (18.5, 18.11);
- the scope line coverage must not be more than 0.50 points below the
  baseline (18.6);
- for pull requests, at least 80 % of the changed src lines that lcov counts
  (DA lines of scope files) must be covered (18.7);
- on a push to refs/heads/main above the baseline, the measured value is
  printed as the next baseline (18.8).

Usage:
    check.py --summary SUMMARY_JSON --lcov LCOV --baseline FILE
             --groups GROUPS_JSON --source-dir REPO [--event-name NAME]
             [--ref REF] [--base-ref REF] [--step-summary FILE]

--event-name, --ref, --base-ref, and --step-summary default to
GITHUB_EVENT_NAME, GITHUB_REF, origin/GITHUB_BASE_REF, and
GITHUB_STEP_SUMMARY. A pull request is --event-name pull_request or a
non-empty --base-ref; changed lines come from
`git diff -U0 <base-ref>...HEAD -- src` in REPO.

Results are printed and appended to the step summary file when one is set.
Exit status: 0 when every rule passes, 1 on any failure, 2 on a usage error.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from decimal import Decimal, ROUND_DOWN

# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402

# Requirement 18.4: first-party directories whose line coverage must be above 0.
REQUIRED_NONZERO_DIRECTORIES = (
    "src/common",
    "src/kernel",
    "src/libs",
    "src/loader",
    "src/graphics/guest_gpu",
    "src/graphics/host_gpu",
    "src/graphics/shader",
    "src/graphics/presentation",
    "src/launcher",
    "src/updater",
)
MAX_REGRESSION = Decimal("0.50")
CHANGED_LINE_THRESHOLD = 80
BASELINE_PATTERN = re.compile(r"[0-9]{1,3}\.[0-9]{2}")
HUNK_PATTERN = re.compile(r"@@ -[0-9]+(?:,[0-9]+)? \+([0-9]+)(?:,([0-9]+))? @@")


class UsageError(Exception):
    """The command line or groups.json cannot be used."""


class Results:
    """Collects PASS, FAIL, and note lines in order."""

    def __init__(self):
        self.lines = []
        self.failures = 0

    def check(self, ok, text):
        self.lines.append(("PASS " if ok else "FAIL ") + text)
        if not ok:
            self.failures += 1

    def fail(self, text):
        self.check(False, text)

    def note(self, text):
        self.lines.append(text)


def fmt(value):
    return f"{value:.2f}"


def _values(entry):
    """Return (covered, total, percent) of one summary metric entry."""
    values = (entry["covered"], entry["total"], entry["percent"])
    if not all(isinstance(value, (int, Decimal)) and not isinstance(value, bool)
               for value in values):
        raise ValueError("a metric entry has non-numeric values")
    return entry["covered"], entry["total"], Decimal(entry["percent"])


def _metric(summary, section, name, metric):
    return _values(summary[section][name][metric])


def load_summary(path):
    """Return the summary parsed with Decimal percents, or raise ValueError."""
    try:
        with open(path, encoding="utf-8") as handle:
            summary = json.load(handle, parse_float=Decimal)
    except OSError as error:
        raise FileNotFoundError(str(error)) from error
    if not isinstance(summary, dict) or not all(
            isinstance(summary.get(key), dict)
            for key in ("scope", "directories", "files", "groups")):
        raise ValueError("expected 'scope', 'directories', 'files', and 'groups' objects")
    try:
        _values(summary["scope"]["lines"])
    except (KeyError, TypeError) as error:
        raise ValueError(f"scope lines entry is incomplete: {error}") from error
    return summary


def read_baseline(path):
    """Return (value, None) for a valid baseline, else (None, reason)."""
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except FileNotFoundError:
        return None, "missing"
    except (OSError, UnicodeDecodeError) as error:
        return None, f"unreadable ({error})"
    lines = [line for line in text.splitlines() if line != ""]
    if len(lines) != 1:
        return None, f"malformed (expected one line, found {len(lines)} non-empty lines)"
    if not BASELINE_PATTERN.fullmatch(lines[0]):
        return None, f"malformed ({lines[0]!r} does not match ^\\d{{1,3}}\\.\\d{{2}}$)"
    value = Decimal(lines[0])
    if value > Decimal("100.00"):
        return None, f"malformed ({lines[0]} is above 100.00)"
    return value, None


def _unquote(path):
    if len(path) >= 2 and path[0] == '"' and path[-1] == '"':
        raw = path[1:-1].encode("utf-8").decode("unicode_escape")
        return raw.encode("latin-1").decode("utf-8", "replace")
    return path


def parse_diff(text):
    """Map each new-side path of a `git diff -U0` to its added line numbers."""
    changed = {}
    current = None
    in_header = False
    for line in text.splitlines():
        if line.startswith("diff --git "):
            current = None
            in_header = True
        elif in_header and line.startswith("+++ "):
            name = _unquote(line[4:].rstrip("\t"))
            current = name[2:] if name.startswith("b/") else None
            if current is not None:
                changed.setdefault(current, set())
        elif line.startswith("@@"):
            in_header = False
            match = HUNK_PATTERN.match(line)
            if match is None or current is None:
                continue
            start = int(match.group(1))
            count = 1 if match.group(2) is None else int(match.group(2))
            changed[current].update(range(start, start + count))
    return changed


def parse_lcov(text, source_dir):
    """Map relative paths to {line: hit count} from lcov SF/DA records."""
    lines = {}
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("SF:"):
            path = report.relative_path(line[3:], source_dir)
            current = None if path is None else lines.setdefault(path, {})
        elif line == "end_of_record":
            current = None
        elif line.startswith("DA:") and current is not None:
            fields = line[3:].split(",")
            if len(fields) < 2:
                continue
            try:
                number, hits = int(fields[0]), int(fields[1])
            except ValueError:
                continue
            current[number] = max(current.get(number, 0), hits)
    return lines


def changed_line_counts(changed, lcov_lines, scope):
    """Return (covered, counted) changed lines that lcov counts in scope files."""
    covered = counted = 0
    for path, numbers in changed.items():
        if path not in scope or path not in lcov_lines:
            continue
        data = lcov_lines[path]
        for number in numbers:
            if number in data:
                counted += 1
                if data[number] > 0:
                    covered += 1
    return covered, counted


def git_diff(source_dir, base_ref):
    command = ["git", "-C", source_dir, "-c", "core.quotePath=false", "diff", "-U0",
               "--no-color", "--no-ext-diff", "--no-textconv", "-M",
               "--src-prefix=a/", "--dst-prefix=b/", f"{base_ref}...HEAD", "--", "src"]
    result = subprocess.run(command, check=False, capture_output=True)
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"git diff {base_ref}...HEAD failed: {message}")
    return result.stdout.decode("utf-8", "surrogateescape")


def check_groups(results, summary, thresholds):
    for name, limits in thresholds.items():
        for metric, limit in limits.items():
            try:
                covered, total, measured = _metric(summary, "groups", name, metric)
            except (KeyError, TypeError, ValueError):
                results.fail(f"group {name} {metric}: missing from the summary "
                             f"(threshold {limit}%)")
                continue
            results.check(measured >= limit,
                          f"group {name} {metric}: measured {fmt(measured)}% "
                          f"({covered}/{total}), threshold {limit}%")


def check_directories(results, summary):
    for name in REQUIRED_NONZERO_DIRECTORIES:
        try:
            covered, total, measured = _metric(summary, "directories", name, "lines")
        except (KeyError, TypeError, ValueError):
            covered, total, measured = 0, 0, Decimal("0.00")
        results.check(covered > 0,
                      f"directory {name} lines: measured {fmt(measured)}% "
                      f"({covered}/{total}), must be above 0")


def check_baseline(results, measured, baseline_path, event_name, ref):
    baseline, reason = read_baseline(baseline_path)
    if baseline is None:
        results.fail(f"baseline {baseline_path} is {reason}; measured scope line "
                     f"coverage {fmt(measured)}%")
        return
    drop = baseline - measured
    results.check(drop <= MAX_REGRESSION,
                  f"scope line coverage: measured {fmt(measured)}%, baseline "
                  f"{fmt(baseline)}% (allowed drop {fmt(MAX_REGRESSION)} points)")
    if event_name == "push" and ref == "refs/heads/main" and measured > baseline:
        results.note(f"next baseline: {fmt(measured)}")


def check_changed_lines(results, args):
    if not args.base_ref:
        raise UsageError("a pull request check needs --base-ref or GITHUB_BASE_REF")
    try:
        with open(args.lcov, encoding="utf-8") as handle:
            lcov_text = handle.read()
    except OSError as error:
        results.fail(f"changed-line coverage: cannot read lcov file {args.lcov}: {error}")
        return
    try:
        changed = parse_diff(git_diff(args.source_dir, args.base_ref))
        scope = set(report.scope_files(args.source_dir))
    except (OSError, RuntimeError, report.InputError) as error:
        results.fail(f"changed-line coverage: {error}")
        return
    covered, counted = changed_line_counts(changed, parse_lcov(lcov_text, args.source_dir), scope)
    if counted == 0:
        results.check(True, "changed-line coverage: no changed line counts toward line coverage")
        return
    value = report.percent(covered, counted)
    passed = covered * 100 >= CHANGED_LINE_THRESHOLD * counted
    measured = f"{fmt(value)}%"
    if not passed and value == CHANGED_LINE_THRESHOLD:
        # The value rounds up to the threshold: print it truncated so it reads as below it.
        # Truncating a value below the threshold never reaches the threshold.
        exact = Decimal(covered) * 100 / Decimal(counted)
        truncated = exact.quantize(Decimal("0.001"), rounding=ROUND_DOWN)
        measured = f"{truncated}% (below {CHANGED_LINE_THRESHOLD}%)"
    results.check(passed,
                  f"changed-line coverage: measured {measured} ({covered}/{counted}), "
                  f"threshold {CHANGED_LINE_THRESHOLD}%")


def run_checks(args, thresholds):
    results = Results()
    try:
        summary = load_summary(args.summary)
    except FileNotFoundError:
        results.fail(f"coverage summary {args.summary} is missing")
        return results
    except ValueError as error:
        results.fail(f"coverage summary {args.summary} is invalid: {error}")
        return results
    covered, total, measured = _values(summary["scope"]["lines"])
    results.note(f"scope line coverage: {fmt(measured)}% ({covered}/{total})")
    check_groups(results, summary, thresholds)
    check_directories(results, summary)
    check_baseline(results, measured, args.baseline, args.event_name, args.ref)
    if args.event_name == "pull_request" or args.base_ref:
        check_changed_lines(results, args)
    return results


def write_step_summary(path, results):
    body = ["## Coverage check", ""]
    body.extend(f"- {line}" for line in results.lines)
    verdict = "failed" if results.failures else "passed"
    body.extend(("", f"Coverage check {verdict} ({results.failures} failures).", ""))
    with open(path, "a", encoding="utf-8") as handle:
        handle.write("\n".join(body))


def parse_args(argv):
    environ = os.environ
    base = environ.get("GITHUB_BASE_REF", "")
    parser = argparse.ArgumentParser(
        description="Check coverage thresholds, the baseline, and changed lines.")
    parser.add_argument("--summary", required=True, help="coverage/summary.json")
    parser.add_argument("--lcov", required=True, help="llvm-cov export -format=lcov output")
    parser.add_argument("--baseline", required=True, help="coverage baseline file")
    parser.add_argument("--groups", required=True, help="groups.json")
    parser.add_argument("--source-dir", required=True, help="repository checkout")
    parser.add_argument("--event-name", default=environ.get("GITHUB_EVENT_NAME", ""),
                        help="workflow event (default: GITHUB_EVENT_NAME)")
    parser.add_argument("--ref", default=environ.get("GITHUB_REF", ""),
                        help="pushed ref (default: GITHUB_REF)")
    parser.add_argument("--base-ref", default=f"origin/{base}" if base else "",
                        help="pull request base (default: origin/GITHUB_BASE_REF)")
    parser.add_argument("--step-summary", default=environ.get("GITHUB_STEP_SUMMARY", ""),
                        help="file to append results to (default: GITHUB_STEP_SUMMARY)")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        _, thresholds = report.load_groups(args.groups)
        results = run_checks(args, thresholds)
    except (report.InputError, UsageError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    for line in results.lines:
        print(line)
    status = 1 if results.failures else 0
    if args.step_summary:
        try:
            write_step_summary(args.step_summary, results)
        except OSError as error:
            print(f"error: cannot write step summary {args.step_summary}: {error}",
                  file=sys.stderr)
            status = 1
    if results.failures:
        print(f"coverage check failed: {results.failures} failures")
    else:
        print("coverage check passed")
    return status


if __name__ == "__main__":
    sys.exit(main())
