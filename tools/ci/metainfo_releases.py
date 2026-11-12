#!/usr/bin/env python3
"""Write and check the <releases> list of the metainfo in CI builds.

Requirement 14.8 and design.md "Release metadata in the metainfo": a commit
cannot contain its own release tag (KytyPS5-GTK-<UTC commit date>-<sha7>), so
the committed metainfo keeps an empty <releases/>. Before flatpak-builder
copies the checkout, the `flatpak` CI job runs

    metainfo_releases.py write --metainfo FILE --releases RELEASES --tag TAG --date YYYY-MM-DD
                               [--type development]

on the checkout's metainfo: without --type on official runs (a push to main in
MrG00gle/KytyPS5-GTK), and with --type development on every other run, because
flatpak-builder-lint rejects an empty release list. RELEASES holds the
repository's GitHub releases as JSON: the REST objects of
`gh api --paginate repos/<repo>/releases`, or one object per line from its
--jq output, with at least tag_name, published_at, draft, and prerelease. The
<releases/> element is replaced with one <release version="<tag>"
date="<YYYY-MM-DD>"/> per published release (not a draft, not a prerelease;
the date is the UTC date of published_at), newest first, plus TAG with --date
at the top. The rest of the file is unchanged byte for byte.
- Official (no --type): TAG's entry has no type (a stable release, published
  that day). When TAG is already published (a rerun), its entry is kept as
  GitHub has it, and a different --date is an error.
- --type development: TAG's entry gets type="development" (the bundle is a
  development snapshot). When TAG is already published (a development run of a
  released commit), the published entries are written as they are and no
  development entry is added.

The `release` job then runs

    metainfo_releases.py check --metainfo FILE --tag TAG

on the metainfo of the bundle it is about to publish: the first <release> of
<releases> must have version TAG, a YYYY-MM-DD date, and no type other than
stable.

Exit status: 0 on success, 1 when the check fails or the list cannot be
written (the metainfo already has release entries, a release is published
twice, TAG's date is older than a published release, or, official, TAG is
published with another date), 2 on a usage or input error (an unreadable or
malformed file).
"""

import argparse
import datetime
import json
import re
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")
PUBLISHED_AT = re.compile(r"([0-9]{4}-[0-9]{2}-[0-9]{2})T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z")
EMPTY_RELEASES = re.compile(r"<releases\s*/>|<releases\s*>\s*</releases\s*>")
QUOTE = {'"': "&quot;"}


class InputError(Exception):
    """An input file cannot be used (exit 2)."""


class ReleaseError(Exception):
    """The release list cannot be written or the check failed (exit 1)."""


def checked_date(text, what):
    if not isinstance(text, str) or not DATE.fullmatch(text):
        raise InputError(f"{what} {text!r} is not a YYYY-MM-DD date")
    try:
        datetime.date.fromisoformat(text)
    except ValueError:
        raise InputError(f"{what} {text!r} is not a valid date") from None
    return text


def read_metainfo(path):
    """Return (text, parsed root) of a metainfo file."""
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise InputError(f"{path} is not well-formed XML: {error}") from error
    if root.tag != "component":
        raise InputError(f"{path}: the root element is <{root.tag}>, not <component>")
    return text, root


def decode_json_values(text):
    """Decode a sequence of JSON values (pages of gh api --paginate, or --jq lines)."""
    decoder = json.JSONDecoder()
    values = []
    index = 0
    while True:
        while index < len(text) and text[index].isspace():
            index += 1
        if index == len(text):
            return values
        value, index = decoder.raw_decode(text, index)
        values.append(value)


def read_published(path):
    """Return [(tag, published_at, date)] of the published releases, newest first."""
    try:
        with open(path, encoding="utf-8") as handle:
            values = decode_json_values(handle.read())
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise InputError(f"{path} is not JSON: {error}") from error
    releases = []
    for value in values:
        releases.extend(value if isinstance(value, list) else [value])
    published = []
    for release in releases:
        if not isinstance(release, dict):
            raise InputError(f"{path}: a release is not a JSON object")
        tag = release.get("tag_name")
        if not isinstance(tag, str) or not tag:
            raise InputError(f"{path}: a release has no tag_name")
        if not isinstance(release.get("draft"), bool) or \
                not isinstance(release.get("prerelease"), bool):
            raise InputError(f"{path}: release {tag} has no boolean draft and prerelease")
        if release["draft"] or release["prerelease"]:
            continue
        published_at = release.get("published_at")
        match = PUBLISHED_AT.fullmatch(published_at) if isinstance(published_at, str) else None
        if not match:
            raise InputError(f"{path}: release {tag} has published_at {published_at!r}, not a UTC "
                             "RFC 3339 time")
        published.append((tag, published_at, checked_date(match.group(1), f"release {tag} date")))
    tags = [tag for tag, _, _ in published]
    twice = sorted({tag for tag in tags if tags.count(tag) > 1})
    if twice:
        raise ReleaseError(f"published more than once: {', '.join(twice)}")
    # RFC 3339 UTC times of one format sort as text.
    return sorted(published, key=lambda release: release[1], reverse=True)


def entries_for(published, tag, date, kind=None):
    """Return [(version, date, type or None)], newest first, with tag on top unless it is
    published already. kind is None (official) or "development"."""
    entries = [(name, day, None) for name, _, day in published]
    for name, day, _ in entries:
        if name == tag:
            if kind is None and day != date:
                raise ReleaseError(f"release {tag} is already published with date {day}, not "
                                   f"{date}")
            return entries
    if entries and date < entries[0][1]:
        raise ReleaseError(f"the date {date} of {tag} is older than the newest published release "
                           f"{entries[0][0]} ({entries[0][1]})")
    return [(tag, date, kind)] + entries


def releases_text(text, match, entries):
    """The <releases> element for the entries, indented like the element it replaces."""
    line_start = text.rfind("\n", 0, match.start()) + 1
    indent = text[line_start:match.start()]
    if indent.strip():
        indent = ""
    lines = ["<releases>"]
    for version, date, kind in entries:
        attribute = escape(version, QUOTE)
        type_text = f' type="{kind}"' if kind else ""
        lines.append(f'{indent}  <release version="{attribute}" date="{date}"{type_text}/>')
    lines.append(f"{indent}</releases>")
    return "\n".join(lines)


def write(args):
    date = checked_date(args.date, "--date")
    text, root = read_metainfo(args.metainfo)
    releases = root.find("releases")
    if releases is None:
        raise ReleaseError(f"{args.metainfo} has no <releases> element")
    if len(releases) or (releases.text or "").strip():
        raise ReleaseError(f"{args.metainfo} already has release entries; the committed metainfo "
                           "keeps an empty <releases/>")
    matches = list(EMPTY_RELEASES.finditer(text))
    if len(matches) != 1:
        raise ReleaseError(f"{args.metainfo}: expected one empty <releases/> element, found "
                           f"{len(matches)}")
    entries = entries_for(read_published(args.releases), args.tag, date, args.type)
    match = matches[0]
    new_text = text[:match.start()] + releases_text(text, match, entries) + text[match.end():]
    ET.fromstring(new_text)  # still well-formed
    try:
        with open(args.metainfo, "w", encoding="utf-8") as handle:
            handle.write(new_text)
    except OSError as error:
        raise InputError(f"cannot write {args.metainfo}: {error}") from error
    print(f"{args.metainfo}: {len(entries)} release entries, newest first:")
    for version, day, kind in entries:
        print(f"  {version} ({day}{', ' + kind if kind else ''})")
    return 0


def check(args):
    _, root = read_metainfo(args.metainfo)
    releases = root.find("releases")
    entries = releases.findall("release") if releases is not None else []
    if not entries:
        raise ReleaseError(f"{args.metainfo} has no <release> entry; expected {args.tag} first")
    newest = entries[0]
    version, date = newest.get("version"), newest.get("date")
    if version != args.tag:
        raise ReleaseError(f"the newest <release> entry of {args.metainfo} is {version!r}, not "
                           f"{args.tag}")
    try:
        checked_date(date, f"release {version} date")
    except InputError as error:
        raise ReleaseError(str(error)) from None
    if newest.get("type", "stable") != "stable":
        raise ReleaseError(f"the newest <release> entry of {args.metainfo} has type "
                           f"{newest.get('type')!r}; a published release must be stable")
    print(f"{args.metainfo}: the newest of {len(entries)} release entries is {version} ({date})")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write the <releases> list into a metainfo file, or check its newest entry.")
    commands = parser.add_subparsers(dest="command", required=True)
    write_parser = commands.add_parser("write", help="replace <releases/> with the release list")
    write_parser.add_argument("--metainfo", required=True, help="the metainfo file (rewritten)")
    write_parser.add_argument("--releases", required=True,
                              help="the GitHub releases as JSON (gh api --paginate output)")
    write_parser.add_argument("--tag", required=True, help="the tag about to be published")
    write_parser.add_argument("--date", required=True, help="its date, YYYY-MM-DD (UTC)")
    write_parser.add_argument("--type", choices=["development"],
                              help="the tag's entry is a development snapshot (non-official runs)")
    check_parser = commands.add_parser("check", help="the newest <release> must be the tag")
    check_parser.add_argument("--metainfo", required=True, help="the metainfo file")
    check_parser.add_argument("--tag", required=True, help="the tag about to be published")
    args = parser.parse_args(argv)
    try:
        return write(args) if args.command == "write" else check(args)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except ReleaseError as error:
        print(f"error: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
