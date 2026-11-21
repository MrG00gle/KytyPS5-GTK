#!/usr/bin/env python3
"""How far the fork is behind upstream KytyPS5, and what merging it would bring.

Usage:
    sync_status.py --repo DIR [--upstream-ref REF] [--fetch [--url URL]] [--forecast]
                   [--unifdef PATH] [--tsv FILE]

REF defaults to refs/upstream/heads/main. With --fetch it first runs, in DIR,

    git fetch --no-tags URL '+refs/heads/*:refs/upstream/heads/*'

(URL defaults to https://github.com/KytyPS5/KytyPS5.git): by URL, so that no remote is added to
the git configuration, and without tags, because src/generate_version.cmake names versions after
the fork's own tags. It fails when refs/tags changed anyway.

It prints the upstream head, the merge base with HEAD (the last upstream commit merged into the
fork), the number of upstream commits that HEAD lacks, the number of fork commits since the merge
base, and git diff --shortstat between the merge base and the upstream head.

--forecast adds what merging REF into HEAD would bring, without changing the index, the work
tree, or the repository's objects (git merge-tree writes into a temporary object directory that
reads the repository's objects as alternates):
- the paths that a plain merge leaves in conflict (git merge-tree --write-tree --name-only
  --messages HEAD REF);
- what platform_merge.py would do (the same rules, computed in memory): the strip-merged C and C++
  paths with conflict hunks and their counts, the removed-platform conditional directives left
  after stripping (path:line), the unifdef -t retries, the fork-owned paths that upstream changed
  with their numbers of upstream commits, the removed-area and fork-deleted paths that upstream
  changed, the paths that upstream deleted and the fork changed, and the other paths that git's
  merge with renames off leaves in conflict.
It needs unifdef (--unifdef, default: unifdef on PATH).

--tsv FILE writes a draft disposition table with the columns of the investigation's
upstream-commits.tsv (n, stage, sha, date, author, subject, files, added, deleted, disposition,
areas, paths): one row per upstream commit after the merge base up to REF, oldest first. The
disposition is `c` when every path is in the old launcher, `a+c` when a commit mixes old-launcher
paths with paths outside the fork-owned and removed areas, `b` when it changes the top-level
CMakeLists.txt, `d` when every path is fork-owned or in a removed area (or it changes no file),
and `a` (take as-is) otherwise. `e` (an owner decision) is never assigned, and the stage column is
left empty.

Exit status: 0 on success, 1 on a git failure or when a fetch changed refs/tags, 2 on a usage
error (REF names no commit, unifdef is missing for --forecast).
"""

import argparse
import os
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import platform_merge  # noqa: E402
import upstream_paths  # noqa: E402
from platform_merge import MergeError, git, output, text  # noqa: E402
from upstream_paths import FORK_OWNED, REMOVED, STRIP  # noqa: E402

NAME = "sync_status"
UPSTREAM_REF = "refs/upstream/heads/main"
UPSTREAM_URL = "https://github.com/KytyPS5/KytyPS5.git"
FETCH_REFSPEC = "+refs/heads/*:refs/upstream/heads/*"
TSV_COLUMNS = ("n", "stage", "sha", "date", "author", "subject", "files", "added", "deleted",
               "disposition", "areas", "paths")
DISPOSITIONS = {
    "a": "a: emulator core, take as-is",
    "a+c": "a+c: emulator part merged; port the Qt launcher part to Launcher_Core/GTK",
    "b": "b: build or test registration; translate into the fork's CMake/tests/CMakeLists.txt",
    "c": "c: Qt launcher only; port to Launcher_Core/GTK",
    "d": "d: fork-owned or removed areas only",
}


class UsageError(Exception):
    """A usage error (exit 2)."""


def fetch(repo, url):
    """Fetch url's branches into refs/upstream/heads/ without tags; print git's messages."""
    before = output(repo, "for-each-ref", "refs/tags")
    status, _ = git(repo, "fetch", "--no-tags", url, FETCH_REFSPEC, codes=(0,))
    after = output(repo, "for-each-ref", "refs/tags")
    if before != after:
        raise MergeError("the fetch changed refs/tags; check the tags with git for-each-ref "
                         "refs/tags")
    print(f"{NAME}: fetched {url} into refs/upstream/heads/ (no tags)")
    return status


def status_lines(repo, head, ref, base):
    def describe(commit):
        line = text(output(repo, "log", "-1", "--format=%h %s (%ad)", "--date=short", commit))
        return line.strip()

    behind = int(text(output(repo, "rev-list", "--count", f"{base}..{ref}")))
    ahead = int(text(output(repo, "rev-list", "--count", f"{base}..{head}")))
    stat = text(output(repo, "diff", "--shortstat", base, ref, "--")).strip() or "no changes"
    return [f"upstream head: {describe(ref)}",
            f"merge base (last synced upstream commit): {describe(base)}",
            f"upstream commits not merged: {behind}",
            f"fork commits since the merge base: {ahead}",
            f"upstream changes since the merge base: {stat}"]


def merge_tree_conflicts(repo, head, ref, renames):
    """The paths that git merge-tree leaves in conflict, with its objects written to a temporary
    directory."""
    objects = text(output(repo, "rev-parse", "--path-format=absolute", "--git-path",
                          "objects")).strip()
    with tempfile.TemporaryDirectory(prefix="sync-status-objects-") as scratch:
        environment = {"GIT_OBJECT_DIRECTORY": scratch,
                       "GIT_ALTERNATE_OBJECT_DIRECTORIES": objects}
        options = ["-c", "core.quotePath=false"]
        if not renames:
            options += ["-c", "merge.renames=false"]
        status, out = git(repo, *options, "merge-tree", "--write-tree", "--name-only",
                          "--messages", head, ref, env=environment, codes=(0, 1))
    if status == 0:
        return []
    lines = text(out).split("\n")
    paths = []
    for line in lines[1:]:
        if not line:
            break
        paths.append(line)
    return paths


def forecast_lines(repo, head, ref, stripper_factory):
    plain = merge_tree_conflicts(repo, head, ref, renames=True)
    no_renames = merge_tree_conflicts(repo, head, ref, renames=False)
    with tempfile.TemporaryDirectory(prefix="sync-status-") as scratch:
        stripper = stripper_factory(scratch)
        blobs = platform_merge.Blobs(repo)
        try:
            _, results = platform_merge.plan(repo, head, ref, stripper, blobs, scratch)
        finally:
            blobs.close()
    handled = {entry.path for entry in results if entry.action != "git"}
    merged = [entry for entry in results if entry.action == "strip-merge"]
    conflicted = [entry for entry in merged if entry.hunks]
    leftovers = [entry for entry in merged if entry.leftovers]
    owned = [entry for entry in results if entry.klass == FORK_OWNED]
    removed = [entry for entry in results if entry.klass == REMOVED]
    fork_deleted = [entry for entry in results
                    if entry.klass not in (REMOVED, FORK_OWNED) and entry.action == "keep-deleted"
                    and entry.status != "D"]
    deleted_changed = [entry for entry in results if entry.klass == STRIP and entry.fork_changed]
    other = [path for path in no_renames if path not in handled]
    plain_set = set(plain)
    lines = [f"forecast: a plain merge (git merge-tree) leaves {len(plain)} paths in conflict"]
    lines += [f"  {path}" for path in plain]
    lines.append(f"forecast: platform_merge.py strip-merges {len(merged)} C/C++ paths, "
                 f"{len(conflicted)} with conflict hunks "
                 f"({sum(entry.hunks for entry in conflicted)} hunks)")
    lines += [f"  {entry.path}: {entry.hunks}" for entry in conflicted]
    lines.append("forecast: removed-platform directives left after stripping: "
                 f"{sum(len(entry.leftovers) for entry in leftovers)} in {len(leftovers)} files")
    lines += [f"  {entry.path}:{number}: {directive}"
              for entry in leftovers for number, directive in entry.leftovers]
    lines.append(f"forecast: unifdef -t retries: {len(stripper.retries)}")
    lines += [f"  {path} ({side}, pass {number})" for path, side, number in stripper.retries]
    lines.append(f"forecast: fork-owned paths changed upstream (fork side kept): {len(owned)}, "
                 f"{sum(1 for entry in owned if entry.path in plain_set)} in conflict in a plain "
                 "merge")
    lines += [f"  {entry.path}: {len(entry.commits)} upstream commits ({entry.note})"
              for entry in owned]
    lines.append(f"forecast: removed-area paths changed upstream (left out): {len(removed)}")
    lines += [f"  {entry.path}: {entry.note}" for entry in removed]
    lines.append(f"forecast: fork-deleted paths changed upstream (kept deleted): "
                 f"{len(fork_deleted)}")
    lines += [f"  {entry.path}" for entry in fork_deleted]
    lines.append("forecast: C/C++ paths deleted upstream and changed in the fork: "
                 f"{len(deleted_changed)}")
    lines += [f"  {entry.path}: {entry.note}" for entry in deleted_changed]
    lines.append(f"forecast: other conflicts (git's merge, renames off): {len(other)}")
    lines += [f"  {path}" for path in other]
    return lines


def disposition(paths):
    old = [path for path in paths if upstream_paths.is_old_launcher(path)]
    rest = [path for path in paths
            if upstream_paths.classify(path) not in (REMOVED, FORK_OWNED)]
    if old:
        return "a+c" if rest else "c"
    if "CMakeLists.txt" in paths:
        return "b"
    if not rest:
        return "d"
    return "a"


def commit_rows(repo, base, ref):
    """[(sha, date, author, subject, [(added, deleted, path)])] after base up to ref, oldest
    first."""
    log = output(repo, "-c", "core.quotePath=false", "log", "--reverse", "--no-renames",
                 "--date=short", "--format=%x01%H%x00%ad%x00%an%x00%s", "--numstat",
                 f"{base}..{ref}", "--")
    rows = []
    for line in text(log).split("\n"):
        if line.startswith("\x01"):
            sha, date, author, title = line[1:].split("\0", 3)
            rows.append((sha, date, author, title, []))
        elif line.strip() and rows:
            added, deleted, path = line.split("\t", 2)
            rows[-1][4].append((0 if added == "-" else int(added),
                                0 if deleted == "-" else int(deleted), path))
    return rows


def write_tsv(repo, base, ref, path):
    lines = ["\t".join(TSV_COLUMNS)]
    rows = commit_rows(repo, base, ref)
    for number, (sha, date, author, title, files) in enumerate(rows, 1):
        paths = sorted(name for _, _, name in files)
        areas = sorted({upstream_paths.area(name) for name in paths})
        cells = (str(number), "", sha, date, author, title, str(len(files)),
                 str(sum(added for added, _, _ in files)),
                 str(sum(deleted for _, deleted, _ in files)), DISPOSITIONS[disposition(paths)],
                 ",".join(areas), ";".join(paths))
        lines.append("\t".join(cell.replace("\t", " ") for cell in cells))
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(rows)


def report(args, stripper_factory):
    repo = os.path.abspath(args.repo)
    if args.fetch:
        fetch(repo, args.url)
    ref = platform_merge.rev(repo, args.upstream_ref)
    if ref is None:
        raise UsageError(f"{args.upstream_ref} names no commit in {repo}; fetch it with --fetch")
    head = platform_merge.rev(repo, "HEAD")
    if head is None:
        raise UsageError(f"{repo} has no HEAD commit")
    base = text(output(repo, "merge-base", head, ref, codes=(0, 1))).strip()
    if not base:
        raise UsageError(f"HEAD and {args.upstream_ref} have no merge base")
    for line in status_lines(repo, head, ref, base):
        print(f"{NAME}: {line}")
    if args.forecast:
        if base == ref:
            print("forecast: nothing to merge")
        else:
            for line in forecast_lines(repo, head, ref, stripper_factory):
                print(line)
    if args.tsv:
        count = write_tsv(repo, base, ref, args.tsv)
        print(f"{NAME}: wrote {count} rows to {args.tsv}")
    return 0


def main(argv=None, stripper_factory=None):
    parser = argparse.ArgumentParser(
        description="Show how far the fork is behind upstream KytyPS5 and forecast the merge.")
    parser.add_argument("--repo", required=True, help="the fork's work tree")
    parser.add_argument("--upstream-ref", default=UPSTREAM_REF,
                        help=f"the upstream branch (default: {UPSTREAM_REF})")
    parser.add_argument("--fetch", action="store_true",
                        help="fetch upstream's branches into refs/upstream/heads/ first")
    parser.add_argument("--url", default=UPSTREAM_URL,
                        help=f"for --fetch (default: {UPSTREAM_URL})")
    parser.add_argument("--forecast", action="store_true",
                        help="forecast the merge of the upstream branch into HEAD")
    parser.add_argument("--unifdef", default="unifdef", help="the unifdef program, for --forecast")
    parser.add_argument("--tsv", help="write a draft disposition table to this file")
    args = parser.parse_args(argv)
    try:
        if args.url != UPSTREAM_URL and not args.fetch:
            raise UsageError("--url needs --fetch")
        if stripper_factory is None and args.forecast:
            try:
                unifdef = platform_merge.find_unifdef(args.unifdef)
            except MergeError as error:
                raise UsageError(str(error)) from None

            def stripper_factory(scratch):
                return platform_merge.UnifdefStripper(unifdef, scratch)
        return report(args, stripper_factory)
    except UsageError as error:
        print(f"{NAME}: error: {error}", file=sys.stderr)
        return 2
    except (MergeError, OSError) as error:
        print(f"{NAME}: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
