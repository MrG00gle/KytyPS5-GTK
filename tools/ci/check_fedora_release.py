#!/usr/bin/env python3
"""Check that an os-release file describes a generally available Fedora release.

Requirement 16.2: the CI_Pipeline builds in a container of the newest Fedora
release that is generally available, excluding Rawhide and Beta releases. The
`fedora` CI job runs in fedora:latest and calls this script on the container's
/etc/os-release before it installs anything, so a fedora:latest tag that ever
points at a development release fails the job instead of passing unnoticed.

The file passes when:
- ID is "fedora";
- VERSION_ID is a release number (digits only);
- RELEASE_TYPE, when present, is "stable";
- none of VERSION, PRETTY_NAME, VARIANT, REDHAT_SUPPORT_PRODUCT_VERSION, and
  REDHAT_BUGZILLA_PRODUCT_VERSION names Rawhide, Beta, or Prerelease (any case).

It prints one "not a GA release: <reason>" line per failed rule, or
"Fedora <VERSION_ID> is a GA release (<PRETTY_NAME>)".

Usage:
    check_fedora_release.py [OS_RELEASE]     (default /etc/os-release)

Exit status: 0 for a GA release, 1 otherwise, 2 when the file cannot be read
or a line is not KEY=value.
"""

import argparse
import re
import shlex
import sys

DEVELOPMENT_WORDS = re.compile(r"(?i)\b(rawhide|beta|prerelease)\b")
WORD_FIELDS = ("VERSION", "PRETTY_NAME", "VARIANT", "REDHAT_SUPPORT_PRODUCT_VERSION",
               "REDHAT_BUGZILLA_PRODUCT_VERSION")
KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class OsReleaseError(Exception):
    """The file cannot be used."""


def parse_os_release(text):
    """Return the KEY=value pairs of an os-release text (shell-style quoting, # comments)."""
    fields = {}
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key, separator, value = stripped.partition("=")
        if not separator or not KEY.fullmatch(key):
            raise OsReleaseError(f"line {number} is not KEY=value: {line!r}")
        try:
            words = shlex.split(value)
        except ValueError as error:
            raise OsReleaseError(f"line {number}: {error}") from error
        fields[key] = " ".join(words)
    return fields


def problems(fields):
    """Return the reasons why the release is not a GA Fedora release (empty when it is)."""
    reasons = []
    if fields.get("ID") != "fedora":
        reasons.append(f"ID is {fields.get('ID', '')!r}, not 'fedora'")
    version_id = fields.get("VERSION_ID", "")
    if not version_id.isascii() or not version_id.isdigit():
        reasons.append(f"VERSION_ID is {version_id!r}, not a release number")
    if "RELEASE_TYPE" in fields and fields["RELEASE_TYPE"] != "stable":
        reasons.append(f"RELEASE_TYPE is {fields['RELEASE_TYPE']!r}, not 'stable'")
    for name in WORD_FIELDS:
        match = DEVELOPMENT_WORDS.search(fields.get(name, ""))
        if match:
            reasons.append(f"{name} is {fields[name]!r}, which names {match.group(1)}")
    return reasons


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Exit 1 unless the os-release file describes a GA Fedora release "
        "(not Rawhide, Beta, or a prerelease).")
    parser.add_argument("os_release", nargs="?", default="/etc/os-release",
                        help="the os-release file (default /etc/os-release)")
    args = parser.parse_args(argv)
    try:
        with open(args.os_release, encoding="utf-8") as handle:
            fields = parse_os_release(handle.read())
    except (OSError, UnicodeDecodeError) as error:
        print(f"error: cannot read {args.os_release}: {error}", file=sys.stderr)
        return 2
    except OsReleaseError as error:
        print(f"error: {args.os_release}: {error}", file=sys.stderr)
        return 2

    reasons = problems(fields)
    for reason in reasons:
        print(f"not a GA release: {reason}")
    if reasons:
        return 1
    print(f"Fedora {fields['VERSION_ID']} is a GA release ({fields.get('PRETTY_NAME', '')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
