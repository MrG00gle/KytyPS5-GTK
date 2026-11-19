#!/usr/bin/env python3
"""Write the text of one GitHub release: the notes written by hand, or the commit subjects.

design.md "CI workflow", release job, and docs/packaging.md "Release notes". The `release` job runs

    release_notes.py --repository OWNER/NAME --tag TAG --sha SHA --releases RELEASES
                     --manual PATH --output FILE [--git-dir DIR]

in a checkout of SHA with the whole history and every tag (actions/checkout with fetch-depth: 0),
and then publish_release.py creates the release with --notes-file FILE. DIR is the checkout
(default: the current directory). RELEASES holds the repository's GitHub releases as JSON, as
metainfo_releases.py reads them (one object per line from the metadata job, with tag_name,
published_at, draft, and prerelease).

1. The previous release is the newest published release in RELEASES (by published_at; drafts and
   prereleases do not count) other than TAG whose tag is in the checkout and is an ancestor of
   SHA. There may be none, for example before the first release.
2. The visible text of PATH (a path in the repository, read from git, not from the work tree) at a
   commit is the file without its HTML comments (<!-- ... -->; an unclosed one hides the rest),
   with the whitespace around it removed. A missing file has none. When the visible text at SHA is
   not empty and differs from the visible text at the previous release (or there is no previous
   release), FILE is that text, as it is: the notes written by hand for this release. So notes
   written for an earlier release are never used again, and nobody has to clear the file after a
   release; editing only the comment does not count as new notes.
3. Otherwise FILE lists the subjects of the commits after the previous release up to SHA (with no
   previous release: up to SHA), without merge commits, at most the newest MAX_SUBJECTS of them,
   oldest first. A subject "<area>: <summary>" (the commit message rules) goes into a group:
   "Launcher" (launcher), "Updater" (updater), "Emulator" (emulator, and every area that is not
   one of the others or a tooling area: the emulator's subsystems such as shader or kernel, whose
   area stays in front of the summary), and "Packaging" (packaging), in that order, each item with
   its first letter capitalized and a link to its commit. The tooling areas (TOOLING) and subjects
   without an area are listed as they are under "Other changes", folded in a <details> block when
   a group above has items. With more than MAX_SUBJECTS commits, a line gives the number of earlier
   commits. With none: "No changes since <previous release>."
4. The generated text ends with the full changelog link: .../compare/<previous>...<TAG>, or
   .../commits/<TAG> without a previous release.

It prints which text it wrote and why, then the text.

Exit status: 0 when FILE is written, 1 on a git or input failure (SHA is not a commit of the
checkout, RELEASES cannot be read or is malformed, PATH at a commit is not a UTF-8 text file, FILE
cannot be written), 2 on a usage error.
"""

import argparse
import os
import re
import subprocess
import sys

# metainfo_releases.py reads the release list the same way for the metainfo.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metainfo_releases  # noqa: E402

NAME = "release_notes"
MAX_SUBJECTS = 100
REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
TAG = re.compile(r"[A-Za-z0-9_.-]+")
FULL_SHA = re.compile(r"[0-9a-f]{40}")
SUBJECT = re.compile(r"([a-z0-9][a-z0-9_-]*): (.+)")
COMMENT = re.compile(r"<!--.*?(?:-->|\Z)", re.DOTALL)
# The user-facing groups in their order: (heading, areas). EMULATOR also takes every area that is
# not in another group or in TOOLING.
GROUPS = (("Launcher", {"launcher"}), ("Updater", {"updater"}), ("Emulator", {"emulator"}),
          ("Packaging", {"packaging"}))
EMULATOR = "Emulator"
# Areas for the people who build and test the project rather than for its users; "test" is the
# singular that older upstream subjects use.
TOOLING = {"tests", "test", "cmake", "build", "coverage", "tools", "ci", "docs", "platform"}


class NotesError(Exception):
    """A git call or an input failed (exit 1)."""


def git(directory, *arguments):
    """Run git in directory; return its standard output as bytes. Raise NotesError on a failure."""
    command = ["git", "-C", directory, *arguments]
    try:
        result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                check=False)
    except OSError as error:
        raise NotesError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise NotesError(f"{' '.join(command)} exited with {result.returncode}: {detail}")
    return result.stdout


def commit_of(directory, revision):
    """The full SHA of the commit that revision names, or None when it names none."""
    command = ["git", "-C", directory, "rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}"]
    try:
        result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                check=False)
    except OSError as error:
        raise NotesError(f"cannot run git: {error}") from error
    sha = result.stdout.decode("utf-8", "replace").strip()
    return sha if result.returncode == 0 and FULL_SHA.fullmatch(sha) else None


def is_ancestor(directory, ancestor, commit):
    command = ["git", "-C", directory, "merge-base", "--is-ancestor", ancestor, commit]
    try:
        result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                check=False)
    except OSError as error:
        raise NotesError(f"cannot run git: {error}") from error
    if result.returncode not in (0, 1):
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise NotesError(f"{' '.join(command)} exited with {result.returncode}: {detail}")
    return result.returncode == 0


def previous_release(directory, published, tag, sha):
    """(tag, commit) of the previous release (step 1), or (None, None); prints each release it
    passes over."""
    for name, _, _ in published:
        if name == tag:
            continue
        commit = commit_of(directory, f"refs/tags/{name}")
        if commit is None:
            print(f"{NAME}: release {name} has no tag in the checkout; not the previous release")
        elif not is_ancestor(directory, commit, sha):
            print(f"{NAME}: release {name} ({commit[:7]}) is not an ancestor of {sha[:7]}; not "
                  "the previous release")
        else:
            return name, commit
    return None, None


def visible_text(directory, commit, path):
    """The visible text of path at commit (step 2): "" when the file does not exist there."""
    listing = git(directory, "ls-tree", "--full-tree", "-z", commit, "--", path)
    entries = [entry for entry in listing.split(b"\0") if entry]
    if not entries:
        return ""
    info, _, name = entries[0].partition(b"\t")
    mode, kind, blob = info.decode("ascii", "replace").split(" ")
    if len(entries) != 1 or name.decode("utf-8", "replace") != path or kind != "blob" \
            or mode == "120000":
        raise NotesError(f"{path} at {commit[:7]} is not a regular file")
    try:
        text = git(directory, "cat-file", "blob", blob).decode("utf-8")
    except UnicodeDecodeError:
        raise NotesError(f"{path} at {commit[:7]} is not UTF-8 text") from None
    return COMMENT.sub("", text.replace("\r\n", "\n")).strip()


def commit_subjects(directory, previous, sha):
    """[(commit, subject)] after previous (None: from the start) up to sha, newest first, without
    merge commits."""
    revisions = [f"{previous}..{sha}"] if previous else [sha]
    output = git(directory, "log", "--no-merges", "--format=%H%x00%s", *revisions, "--")
    found = []
    for line in output.decode("utf-8", "replace").splitlines():
        commit, _, subject = line.partition("\0")
        if commit:
            found.append((commit, subject))
    return found


def escaped(text):
    """Text that Markdown shows as it is where a subject could look like HTML."""
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def capitalized(text):
    return text[:1].upper() + text[1:]


def group_of(subject):
    """(heading of a user-facing group or None for Other changes, the item's text)."""
    match = SUBJECT.fullmatch(subject)
    if not match or match.group(1) in TOOLING:
        return None, subject
    area, summary = match.groups()
    for heading, areas in GROUPS:
        if area in areas:
            return heading, capitalized(summary)
    return EMULATOR, capitalized(subject)


def generated_notes(repository, tag, previous, commits):
    """The text of step 3 and 4 for commits (newest first)."""
    url = f"https://github.com/{repository}"
    shown = list(reversed(commits[:MAX_SUBJECTS]))
    earlier = len(commits) - len(shown)
    groups = {heading: [] for heading, _ in GROUPS}
    other = []
    for commit, subject in shown:
        heading, text = group_of(subject)
        item = f"- {escaped(text)} ([{commit[:7]}]({url}/commit/{commit}))"
        (groups[heading] if heading else other).append(item)
    lines = []
    if not shown:
        lines += [f"No changes since {previous}.", ""]
    for heading, _ in GROUPS:
        if groups[heading]:
            lines += [f"## {heading}", "", *groups[heading], ""]
    if other and any(groups.values()):
        lines += ["<details>", f"<summary>Other changes ({len(other)})</summary>", "", *other, "",
                  "</details>", ""]
    elif other:
        lines += ["## Other changes", "", *other, ""]
    if earlier:
        commits_word = "commit" if earlier == 1 else "commits"
        lines += [f"\u2026 and {earlier} earlier {commits_word}; see the full changelog.", ""]
    if previous:
        lines.append(f"**Full Changelog**: {url}/compare/{previous}...{tag}")
    else:
        lines.append(f"**Full Changelog**: {url}/commits/{tag}")
    return "\n".join(lines) + "\n"


def write_notes(args):
    directory = args.git_dir
    sha = commit_of(directory, args.sha)
    if sha is None:
        raise NotesError(f"{args.sha} is not a commit of the checkout {directory}")
    try:
        published = metainfo_releases.read_published(args.releases)
    except (metainfo_releases.InputError, metainfo_releases.ReleaseError) as error:
        raise NotesError(str(error)) from None
    previous, previous_commit = previous_release(directory, published, args.tag, sha)
    if previous:
        print(f"{NAME}: the previous release is {previous} ({previous_commit[:7]})")
    else:
        print(f"{NAME}: no previous release is an ancestor of {sha[:7]}")

    manual = visible_text(directory, sha, args.manual)
    before = visible_text(directory, previous_commit, args.manual) if previous else ""
    if manual and (not previous or manual != before):
        why = f"its text changed since {previous}" if previous else "there is no previous release"
        notes = manual + "\n"
        print(f"{NAME}: the notes written by hand in {args.manual}, because {why}")
    else:
        why = (f"{args.manual} has the same text as at {previous}" if manual else
               f"{args.manual} has no text outside comments")
        commits = commit_subjects(directory, previous_commit, sha)
        since = f"since {previous}" if previous else f"up to {sha[:7]}"
        shown = min(len(commits), MAX_SUBJECTS)
        notes = generated_notes(args.repository, args.tag, previous, commits)
        print(f"{NAME}: the subjects of {shown} of {len(commits)} commits {since}, because {why}")
    try:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(notes)
    except OSError as error:
        raise NotesError(f"cannot write {args.output}: {error}") from error
    print(f"{NAME}: wrote {args.output}:")
    print(notes, end="")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write a release's text: the notes written by hand when they changed since "
        "the previous release, otherwise the commit subjects since then.")
    parser.add_argument("--repository", required=True, help="OWNER/NAME, for the links")
    parser.add_argument("--tag", required=True, help="the tag of the release to write")
    parser.add_argument("--sha", required=True, help="the release's commit (40 hex digits)")
    parser.add_argument("--releases", required=True,
                        help="the GitHub releases as JSON (the metadata job's list)")
    parser.add_argument("--manual", required=True,
                        help="the notes file, as a path in the repository")
    parser.add_argument("--output", required=True, help="the file to write")
    parser.add_argument("--git-dir", default=".",
                        help="the checkout (git -C DIR; default: the current directory)")
    args = parser.parse_args(argv)
    usage = []
    if not REPOSITORY.fullmatch(args.repository):
        usage.append(f"--repository {args.repository!r} is not OWNER/NAME")
    if not TAG.fullmatch(args.tag):
        usage.append(f"--tag {args.tag!r} is not a release tag")
    if not FULL_SHA.fullmatch(args.sha):
        usage.append(f"--sha {args.sha!r} is not a full commit SHA (40 lowercase hex digits)")
    manual = args.manual.split("/")
    if args.manual.startswith("/") or any(part in ("", ".", "..") for part in manual):
        usage.append(f"--manual {args.manual!r} is not a relative path in the repository")
    for problem in usage:
        print(f"error: {problem}", file=sys.stderr)
    if usage:
        return 2
    try:
        return write_notes(args)
    except NotesError as error:
        print(f"error: {error}")
        print(f"FAILED: {NAME}: {args.output} was not written")
        return 1


if __name__ == "__main__":
    sys.exit(main())
