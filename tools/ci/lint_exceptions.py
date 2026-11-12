#!/usr/bin/env python3
"""Write a per-run copy of the flatpak-builder-lint exceptions file with more IDs.

design.md (the lint-exceptions.json bullet): on runs whose ref is not
refs/heads/main, the `flatpak` CI job lints with a copy of
packaging/flatpak/lint-exceptions.json that also excepts
appstream-missing-screenshots, because the metainfo's screenshot URL points at
main and resolves only after the merge. Runs on main lint with the committed
file unchanged; the workflow decides which file a run uses, and this script
never writes the file it reads.

Usage:
    lint_exceptions.py --exceptions FILE --app-id APP_ID --add LINT_ID [--add ...]
                       --output FILE

The file has flatpak-builder-lint's --user-exceptions format: a JSON object
whose keys are App_IDs and whose values are lists of lint IDs. The copy keeps
every entry and appends each --add ID to the App_ID's list unless it is listed
already. It prints the App_ID's resulting list.

Exit status: 0 on success, 2 on a usage or input error (an unreadable or
malformed file, no list for the App_ID, a lint ID other than lowercase letters,
digits, and "-", or --output naming the --exceptions file).
"""

import argparse
import json
import os
import re
import sys

LINT_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")


class InputError(Exception):
    """The input cannot be used."""


def read_exceptions(path, app_id):
    try:
        with open(path, encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise InputError(f"{path} is not JSON: {error}") from error
    if not isinstance(document, dict):
        raise InputError(f"{path} is not a JSON object of App_IDs")
    ids = document.get(app_id)
    if not isinstance(ids, list) or not all(isinstance(entry, str) for entry in ids):
        raise InputError(f"{path} has no list of lint IDs for {app_id}")
    return document


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Copy a flatpak-builder-lint exceptions file and add lint IDs for one App_ID.")
    parser.add_argument("--exceptions", required=True, help="the committed exceptions file")
    parser.add_argument("--app-id", required=True, help="the App_ID whose list gets the IDs")
    parser.add_argument("--add", required=True, action="append", metavar="LINT_ID",
                        help="a lint ID to add (repeatable)")
    parser.add_argument("--output", required=True, help="the per-run copy to write")
    args = parser.parse_args(argv)
    try:
        if os.path.abspath(args.output) == os.path.abspath(args.exceptions):
            raise InputError("--output must not be the --exceptions file; the committed file "
                             "stays unchanged")
        for lint_id in args.add:
            if not LINT_ID.fullmatch(lint_id):
                raise InputError(f"{lint_id!r} is not a lint ID")
        document = read_exceptions(args.exceptions, args.app_id)
        ids = document[args.app_id]
        for lint_id in args.add:
            if lint_id not in ids:
                ids.append(lint_id)
        try:
            with open(args.output, "w", encoding="utf-8") as handle:
                json.dump(document, handle, indent=4)
                handle.write("\n")
        except OSError as error:
            raise InputError(f"cannot write {args.output}: {error}") from error
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"{args.output}: {args.app_id}: {', '.join(ids)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
