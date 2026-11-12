#!/usr/bin/env python3
"""Check that the running guide names every option of `kyty_emulator --help` (task 29.6,
Requirements 20.3 and 20.12).

The check runs `<emulator> --help` with its standard input from /dev/null and a timeout (5 s by
default) and reads every option name in its standard output: a word that begins with "--" and a
letter or digit, up to the first character that is not a letter, digit, "_", or "-". So
"--game <dir>" and "--keymap=<x>" give --game and --keymap, and a description that mentions
another option counts too. Each option name must appear verbatim in the guide as a whole word, not
only inside a longer option name: "--gpu" inside "--gpu-assisted-validation" does not count.

Usage:
    check_help_options.py --emulator EXE --doc FILE [--timeout SECONDS]

The check fails when the emulator cannot be started, does not exit within the timeout, exits with
a non-zero status, or prints no option name, and when the guide omits an option. It prints each
missing option on its own line, then one summary line, `FAILED: docs_help_options: ...` or
`PASS: docs_help_options: ...`.

Exit status: 0 when the guide names every option, 1 when the check fails, 2 on a usage error or an
unreadable guide.
"""

import argparse
import os
import re
import signal
import subprocess
import sys

NAME = "docs_help_options"
DEFAULT_TIMEOUT = 5.0
OPTION = re.compile(r"(?<![A-Za-z0-9_-])--[A-Za-z0-9][A-Za-z0-9_-]*")
# Lines of the emulator's standard error that a failure shows.
STDERR_LINES = 20


class CheckFailed(Exception):
    """The emulator's --help cannot be used."""


def help_options(stdout):
    """The option names in the --help output, in the order of their first appearance."""
    return list(dict.fromkeys(OPTION.findall(stdout)))


def names_option(doc, option):
    return re.search(rf"(?<![A-Za-z0-9_-]){re.escape(option)}(?![A-Za-z0-9_-])", doc) is not None


def run_help(emulator, timeout):
    """The standard output of `<emulator> --help`."""
    try:
        # A session of its own, so that a timeout kills every process the emulator started.
        process = subprocess.Popen([emulator, "--help"], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
    except OSError as error:
        raise CheckFailed(f"cannot run {emulator}: {error.strerror or error}") from None
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise CheckFailed(f"{emulator} --help did not exit within {timeout:g} s") from None
    stdout = stdout.decode("utf-8", "replace")
    if process.returncode != 0:
        lines = stderr.decode("utf-8", "replace").splitlines()[:STDERR_LINES]
        status = (f"was killed by signal {-process.returncode}" if process.returncode < 0
                  else f"exited with status {process.returncode}")
        raise CheckFailed("\n".join([*lines, f"{emulator} --help {status}"]))
    return stdout


def positive_seconds(value):
    try:
        seconds = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{value!r} is not a number of seconds") from None
    if not seconds > 0:
        raise argparse.ArgumentTypeError(f"{value!r} is not a positive number of seconds")
    return seconds


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check that the running guide names every option that the emulator's --help "
        "prints.")
    parser.add_argument("--emulator", required=True, help="the kyty_emulator executable")
    parser.add_argument("--doc", required=True, help="the running guide (docs/running.md)")
    parser.add_argument("--timeout", type=positive_seconds, default=DEFAULT_TIMEOUT,
                        help=f"seconds that --help may take (default {DEFAULT_TIMEOUT:g})")
    args = parser.parse_args(argv)
    try:
        with open(args.doc, encoding="utf-8") as handle:
            doc = handle.read()
    except (OSError, UnicodeDecodeError) as error:
        print(f"error: {NAME}: cannot read {args.doc}: {error}", file=sys.stderr)
        return 2
    try:
        options = help_options(run_help(args.emulator, args.timeout))
        if not options:
            raise CheckFailed(f"{args.emulator} --help printed no option name (a word that "
                              "begins with --)")
    except CheckFailed as failure:
        print(failure)
        print(f"FAILED: {NAME}: the emulator's --help cannot be checked (Requirement 20.12)")
        return 1
    missing = [option for option in options if not names_option(doc, option)]
    for option in missing:
        print(f"{args.doc} lacks the option {option}")
    if missing:
        print(f"FAILED: {NAME}: {len(missing)} of the {len(options)} options of "
              f"{os.path.basename(args.emulator)} --help missing from {args.doc} "
              "(Requirements 20.3, 20.12)")
        return 1
    print(f"PASS: {NAME}: all {len(options)} options of {os.path.basename(args.emulator)} --help "
          f"appear in {args.doc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
