"""A stand-in for the GitHub CLI in the publish_release.py tests.

The tests put a `gh` wrapper that runs this file first on PATH. It keeps the
repository's releases in the JSON file FAKE_GH_STATE ({"next_id": n,
"releases": [...]}, REST release objects), appends each call's arguments as
one JSON line to FAKE_GH_LOG, and answers only the calls publish_release.py
makes:

    gh api --paginate repos/<repo>/releases?per_page=100
    gh api -X DELETE repos/<repo>/releases/<id>
    gh release create <tag> --repo <repo> --draft --target <sha> --title <t> --generate-notes
    gh release upload <tag> <file>... --repo <repo>
    gh release edit <tag> --repo <repo> --draft=false --latest

The list prints two releases per page, one JSON array after another, like
`gh api --paginate`. FAKE_GH_FAIL names one step that fails with exit 1:
list (every list call), create, upload (after the first file is attached),
corrupt (every upload is attached with a wrong digest; the call succeeds),
edit, or delete. Any other call exits 2.
"""

import hashlib
import json
import os
import sys


def load():
    with open(os.environ["FAKE_GH_STATE"], encoding="utf-8") as handle:
        return json.load(handle)


def save(state):
    with open(os.environ["FAKE_GH_STATE"], "w", encoding="utf-8") as handle:
        json.dump(state, handle, indent=1)


def fail(message, status=1):
    print(message, file=sys.stderr)
    sys.exit(status)


def option(arguments, name):
    index = arguments.index(name)
    return arguments[index + 1]


def release_for(state, tag):
    """The draft for the tag, else the published release (gh's own lookup order is similar)."""
    candidates = [release for release in state["releases"] if release["tag_name"] == tag]
    candidates.sort(key=lambda release: not release["draft"])
    if not candidates:
        fail("release not found")
    return candidates[0]


def asset(path, corrupt):
    with open(path, "rb") as handle:
        data = handle.read()
    digest = hashlib.sha256(data + (b"x" if corrupt else b"")).hexdigest()
    return {"name": os.path.basename(path), "size": len(data), "digest": f"sha256:{digest}",
            "state": "uploaded"}


def main(arguments):
    with open(os.environ["FAKE_GH_LOG"], "a", encoding="utf-8") as handle:
        handle.write(json.dumps(arguments) + "\n")
    failing = os.environ.get("FAKE_GH_FAIL", "")
    state = load()

    if arguments[:2] == ["api", "--paginate"] and arguments[2].endswith("/releases?per_page=100"):
        if failing == "list":
            fail("gh: Server Error (HTTP 500)")
        releases = state["releases"]
        for start in range(0, max(len(releases), 1), 2):
            print(json.dumps(releases[start:start + 2], indent=1 if start % 4 else None))
        return 0

    if arguments[:3] == ["api", "-X", "DELETE"]:
        if failing == "delete":
            fail("gh: Server Error (HTTP 500)")
        release_id = int(arguments[3].rsplit("/", 1)[1])
        remaining = [release for release in state["releases"] if release["id"] != release_id]
        if len(remaining) == len(state["releases"]):
            fail("gh: Not Found (HTTP 404)")
        state["releases"] = remaining
        save(state)
        return 0

    if arguments[:2] == ["release", "create"]:
        if failing == "create":
            fail("HTTP 422: Validation Failed")
        release = {"id": state["next_id"], "tag_name": arguments[2],
                   "name": option(arguments, "--title"),
                   "target_commitish": option(arguments, "--target"),
                   "draft": "--draft" in arguments, "prerelease": False, "latest": False,
                   "assets": []}
        state["next_id"] += 1
        state["releases"].insert(0, release)
        save(state)
        return 0

    if arguments[:2] == ["release", "upload"]:
        release = release_for(state, arguments[2])
        files = arguments[3:arguments.index("--repo")]
        for number, path in enumerate(files):
            if failing == "upload" and number == 1:
                save(state)
                fail(f"upload of {os.path.basename(path)} failed: HTTP 502")
            release["assets"].append(asset(path, failing == "corrupt"))
        save(state)
        return 0

    if arguments[:2] == ["release", "edit"]:
        if failing == "edit":
            fail("HTTP 500: Server Error")
        release = release_for(state, arguments[2])
        if "--draft=false" in arguments:
            release["draft"] = False
        if "--latest" in arguments:
            for other in state["releases"]:
                other["latest"] = False
            release["latest"] = True
        save(state)
        return 0

    fail(f"fake gh: unexpected call {arguments}", 2)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
