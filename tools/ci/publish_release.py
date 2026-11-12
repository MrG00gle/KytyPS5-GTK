#!/usr/bin/env python3
"""Publish the GitHub release of one CI build, all assets or nothing.

Requirements 16.9, 16.12, and 16.13; design.md "CI workflow", release job. The
`release` job runs this only on a push to main in MrG00gle/KytyPS5-GTK, after
every other job succeeded and after it wrote the .sha256 files. It uses the
GitHub CLI (`gh`, authenticated through GH_TOKEN) on the repository given with
--repo:

1. List the repository's releases (`gh api --paginate`) and take those whose
   tag_name is the tag.
2. A published (non-draft) release for the tag that is not a prerelease and
   holds exactly the given assets, each with the local file's size and, when
   GitHub reports one, its sha256 digest: nothing to do, exit 0. This makes a
   rerun of a completed job succeed without changes.
3. A published release for the tag that differs from that: exit 1 and change
   nothing (a published release is never deleted by this script).
4. Delete every leftover draft for the tag (an earlier run that stopped).
5. Create a draft (`gh release create <tag> --draft --target <sha>`), upload
   every asset (`gh release upload`), list the releases again and check that
   the draft holds exactly the assets with the right sizes and digests, then
   publish it as the latest release (`gh release edit --draft=false --latest`).
6. When step 5 fails: delete every draft for the tag and exit 1, so no
   incomplete release is left behind. A draft is not visible as a published
   release, and GitHub creates the tag only when the release is published.

Usage:
    publish_release.py --repo OWNER/NAME --tag TAG --target SHA ASSET...

The release title is the tag; its notes are GitHub's generated notes.
Exit status: 0 when the release is published and complete, 1 on any failure,
2 on a usage error (a missing or empty asset, two assets with one name).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

GH = "gh"


class GhError(Exception):
    """A gh call failed or returned something unusable."""


def run_gh(arguments):
    """Run gh with the arguments; return its standard output. Raise GhError on a failure."""
    command = [GH, *arguments]
    print("+ " + " ".join(command), flush=True)
    try:
        result = subprocess.run(command, capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                check=False)
    except OSError as error:
        raise GhError(f"cannot run {GH}: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise GhError(f"{' '.join(command)} exited with {result.returncode}: {detail}")
    return result.stdout


def decode_json_values(text):
    """Decode a sequence of JSON values (gh api --paginate prints one array per page)."""
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


def releases_for_tag(repo, tag):
    """Return the releases (REST objects) of repo whose tag_name is tag, drafts included."""
    output = run_gh(["api", "--paginate", f"repos/{repo}/releases?per_page=100"])
    try:
        pages = decode_json_values(output)
    except json.JSONDecodeError as error:
        raise GhError(f"the release list is not JSON: {error}") from error
    releases = []
    for page in pages:
        if not isinstance(page, list):
            raise GhError("the release list is not a JSON array")
        for release in page:
            if not isinstance(release, dict) or "id" not in release:
                raise GhError("a release in the list has no id")
            if release.get("tag_name") == tag:
                releases.append(release)
    return releases


def local_asset(path):
    """Return (name, size, sha256 hex digest) of a local asset file."""
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
            size += len(block)
    return os.path.basename(path), size, digest.hexdigest()


def asset_problems(release, expected):
    """Return what differs between a release's assets and the expected {name: (size, hex)}."""
    found = {}
    reasons = []
    for asset in release.get("assets") or []:
        name = asset.get("name")
        if name in found:
            reasons.append(f"asset {name} appears twice")
        found[name] = asset
    for name in sorted(set(expected) - set(found)):
        reasons.append(f"asset {name} is missing")
    for name in sorted(set(found) - set(expected)):
        reasons.append(f"asset {name} is not one of this build's assets")
    for name in sorted(set(expected) & set(found)):
        size, hexdigest = expected[name]
        asset = found[name]
        if asset.get("state", "uploaded") != "uploaded":
            reasons.append(f"asset {name} is in state {asset.get('state')!r}")
        if asset.get("size") != size:
            reasons.append(f"asset {name} has {asset.get('size')} bytes, the build's file {size}")
        digest = asset.get("digest")
        if digest is not None and digest.lower() != f"sha256:{hexdigest}":
            reasons.append(f"asset {name} has digest {digest}, the build's file sha256:{hexdigest}")
    return reasons


def delete_drafts(repo, tag):
    """Delete every draft release for the tag; return how many were deleted."""
    drafts = [release for release in releases_for_tag(repo, tag) if release.get("draft")]
    for release in drafts:
        run_gh(["api", "-X", "DELETE", f"repos/{repo}/releases/{release['id']}"])
    return len(drafts)


def publish(repo, tag, target, paths, expected):
    """Steps 1 to 6 of the module description. Return the exit status."""
    existing = releases_for_tag(repo, tag)
    published = [release for release in existing if not release.get("draft")]
    if published:
        release = published[0]
        reasons = asset_problems(release, expected)
        if release.get("prerelease"):
            reasons.insert(0, "it is a prerelease")
        if len(published) > 1:
            reasons.insert(0, f"{len(published)} published releases have this tag")
        if reasons:
            for reason in reasons:
                print(f"error: the published release {tag}: {reason}")
            print(f"FAILED: release {tag} is published but differs from this build; nothing "
                  "was changed")
            return 1
        print(f"Release {tag} is already published with all {len(expected)} assets; nothing to do.")
        return 0

    deleted = delete_drafts(repo, tag) if existing else 0
    if deleted:
        print(f"Deleted {deleted} leftover draft release(s) for {tag}.")
    try:
        run_gh(["release", "create", tag, "--repo", repo, "--draft", "--target", target,
                "--title", tag, "--generate-notes"])
        run_gh(["release", "upload", tag, *paths, "--repo", repo])
        drafts = [release for release in releases_for_tag(repo, tag) if release.get("draft")]
        if len(drafts) != 1:
            raise GhError(f"expected one draft release for {tag} after the upload, found "
                          f"{len(drafts)}")
        reasons = asset_problems(drafts[0], expected)
        if reasons:
            raise GhError("the draft's assets differ from the build's files: " + "; ".join(reasons))
        run_gh(["release", "edit", tag, "--repo", repo, "--draft=false", "--latest"])
    except GhError as error:
        print(f"error: {error}")
        try:
            deleted = delete_drafts(repo, tag)
            print(f"Deleted {deleted} draft release(s) for {tag}; no incomplete release is left.")
        except GhError as cleanup_error:
            print(f"error: deleting the draft release of {tag} failed too: {cleanup_error}")
        print(f"FAILED: release {tag} was not published")
        return 1
    print(f"Published release {tag} as the latest release with {len(expected)} assets.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Create a draft release, upload every asset, check them, and publish the "
        "release as latest; delete the draft on any failure.")
    parser.add_argument("--repo", required=True, help="OWNER/NAME of the repository")
    parser.add_argument("--tag", required=True, help="the release tag (also the title)")
    parser.add_argument("--target", required=True, help="the commit the tag will point at")
    parser.add_argument("assets", nargs="+", help="the files to attach")
    args = parser.parse_args(argv)

    expected = {}
    for path in args.assets:
        try:
            name, size, hexdigest = local_asset(path)
        except OSError as error:
            print(f"error: cannot read the asset {path}: {error}", file=sys.stderr)
            return 2
        if size == 0:
            print(f"error: the asset {path} is empty", file=sys.stderr)
            return 2
        if name in expected:
            print(f"error: two assets are named {name}", file=sys.stderr)
            return 2
        expected[name] = (size, hexdigest)

    try:
        return publish(args.repo, args.tag, args.target, args.assets, expected)
    except (GhError, json.JSONDecodeError) as error:
        print(f"error: {error}")
        print(f"FAILED: release {args.tag} was not published")
        return 1


if __name__ == "__main__":
    sys.exit(main())
