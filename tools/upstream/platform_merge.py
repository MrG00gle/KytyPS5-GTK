#!/usr/bin/env python3
"""Merge an upstream KytyPS5 commit and strip the removed platforms from what it brings.

Usage:
    platform_merge.py --repo DIR --report FILE --auto-dir DIR [--unifdef PATH] COMMIT

It refuses (exit 2) when DIR has uncommitted changes or untracked files, when a merge is in
progress, when COMMIT is already merged, when unifdef cannot be found, or when the --auto-dir
directory is not empty. Otherwise it runs, in DIR,

    git -c merge.renames=false merge --no-ff --no-commit COMMIT

(renames off, so that git's merge and the per-path rules below see the same paths), and then
goes over every path that changed between the merge base and COMMIT (git diff --no-renames), in
the classes of upstream_paths.py:

- removed: stays as the fork has it (deleted); an upstream addition is left out.
- fork-owned: the fork's side is kept (an upstream addition is left out), and the report lists
  the upstream commits that change the path, for translation by hand.
- a path that the fork deleted and upstream changed stays deleted.
- strip (C and C++ under src/ and tests/):
    deleted upstream   deleted; the report says whether the fork changed it since the merge base
                       and where upstream renamed it to, if it did;
    added upstream     strip(upstream), or a merge with an empty base when the fork added it too;
    changed upstream   git merge-file -p -L ours -L base -L upstream
                         <fork's file> strip(<merge base's file>) strip(<upstream's file>)
  where strip runs `unifdef -m -U_WIN32 -UWIN32 -U_MSC_VER -U__MINGW32__ -U__MINGW64__
  -U__APPLE__ -UKYTY_LINKED_GUEST_ADDRESS_SPACE`, then `unifdef -m -DKYTY_PLATFORM=5
  -DKYTY_PLATFORM_WINDOWS=1`, and runs a pass again with -t when unifdef exits 2 on it (digit
  separators such as 1'000 read as an unterminated character literal). A clean result is written
  and staged with git add; a result with conflict hunks is written with its markers and left
  unmerged (index stages 1-3 hold the stripped base, the fork's file, and the stripped upstream
  file). Every computed result is also copied to the --auto-dir directory.
- other: git's own merge result; its conflicts are listed.

The report (Markdown) gives the summary counts, the removed-area paths, the fork-deleted paths,
the fork-owned paths with their upstream commits, the strip-merged paths with their conflict hunks,
the removed-platform conditional directives left in the strip-merged results (path:line, the
macros of source_check_platform_macros), the unifdef -t retries, the paths deleted upstream, the
other conflicts, and every file upstream added.

Exit status: 0 when nothing is left to do by hand, 1 when hand work remains (conflict hunks,
leftover directives, fork-owned paths with upstream changes, paths deleted upstream that the fork
changed, or other conflicts), 2 on a refusal or a usage, git, or unifdef error. After an exit 1 or
0 the merge is in progress: finish it with git commit, or drop it with git merge --abort.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upstream_paths  # noqa: E402
from upstream_paths import FORK_OWNED, OTHER, REMOVED, STRIP  # noqa: E402

NAME = "platform_merge"
UNIFDEF_PASSES = (
    ("-U_WIN32", "-UWIN32", "-U_MSC_VER", "-U__MINGW32__", "-U__MINGW64__", "-U__APPLE__",
     "-UKYTY_LINKED_GUEST_ADDRESS_SPACE"),
    ("-DKYTY_PLATFORM=5", "-DKYTY_PLATFORM_WINDOWS=1"),
)
LABELS = ("ours", "base", "upstream")
CONFLICT_START = b"<<<<<<< ours"
# Fixed, predictable git output: no pager, no locale, no editor, paths taken literally.
GIT_ENVIRONMENT = {"LC_ALL": "C", "GIT_PAGER": "cat", "GIT_EDITOR": "true",
                   "GIT_MERGE_AUTOEDIT": "no", "GIT_LITERAL_PATHSPECS": "1"}
REGULAR_MODES = ("100644", "100755")


class MergeError(Exception):
    """A git or unifdef failure, or a refusal (exit 2)."""


def git(repo, *arguments, stdin=None, env=None, codes=(0,)):
    """Run git in repo; return (exit status, stdout bytes). Raise MergeError when the status is
    not in codes."""
    environment = dict(os.environ, **GIT_ENVIRONMENT, **(env or {}))
    command = ["git", "-C", repo, *arguments]
    try:
        result = subprocess.run(command, input=stdin, capture_output=True, env=environment,
                                stdin=None if stdin is not None else subprocess.DEVNULL,
                                check=False)
    except OSError as error:
        raise MergeError(f"cannot run git: {error}") from error
    if result.returncode not in codes:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise MergeError(f"{' '.join(command)} exited with {result.returncode}: {detail}")
    return result.returncode, result.stdout


def output(repo, *arguments, **options):
    return git(repo, *arguments, **options)[1]


def text(data):
    return data.decode("utf-8", "replace")


def rev(repo, revision):
    """The full SHA of the commit that revision names, or None."""
    status, out = git(repo, "rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}",
                      codes=(0, 1, 128))
    return text(out).strip() if status == 0 else None


def subject(repo, commit):
    return text(output(repo, "log", "-1", "--format=%s", commit)).strip()


def tree(repo, commit):
    """{path: (mode, type, object)} of every entry of commit's tree, recursively."""
    entries = {}
    for entry in output(repo, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not entry:
            continue
        info, _, path = entry.partition(b"\t")
        mode, kind, obj = text(info).split(" ")
        entries[text(path)] = (mode, kind, obj)
    return entries


def changed_paths(repo, base, commit):
    """[(status letter, path)] that differ between base and commit, without rename detection."""
    words = output(repo, "diff", "--name-status", "-z", "--no-renames", base, commit, "--")
    words = [text(word) for word in words.split(b"\0") if word]
    return [(words[index][0], words[index + 1]) for index in range(0, len(words) - 1, 2)]


def renames(repo, base, commit):
    """{old path: new path} of the renames that git finds between base and commit."""
    words = output(repo, "diff", "--name-status", "-z", "-M", "--diff-filter=R", base, commit,
                   "--")
    words = [text(word) for word in words.split(b"\0") if word]
    return {words[index + 1]: words[index + 2] for index in range(0, len(words) - 2, 3)}


class Blobs:
    """Reads blobs through one git cat-file --batch process."""

    def __init__(self, repo):
        environment = dict(os.environ, **GIT_ENVIRONMENT)
        try:
            self.process = subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"],
                                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.DEVNULL, env=environment)
        except OSError as error:
            raise MergeError(f"cannot run git: {error}") from error

    def read(self, obj):
        self.process.stdin.write(obj.encode("ascii") + b"\n")
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            raise MergeError(f"git cat-file: {obj} is not a blob")
        data = self.process.stdout.read(int(header[2]))
        self.process.stdout.read(1)
        return data

    def close(self):
        self.process.stdin.close()
        self.process.wait()
        self.process.stdout.close()


class UnifdefStripper:
    """strip(path, side, data): the two phase 2 unifdef passes over data. A pass that exits 2 runs
    again with -t; retries lists each such (path, side, pass number)."""

    def __init__(self, unifdef, scratch):
        self.unifdef = unifdef
        self.scratch = scratch
        self.retries = []

    def run_pass(self, flags, data, retry):
        name = os.path.join(self.scratch, "strip.txt")
        with open(name, "wb") as handle:
            handle.write(data)
        command = [self.unifdef, "-m", *(("-t",) if retry else ()), *flags, name]
        try:
            result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                    check=False)
        except OSError as error:
            raise MergeError(f"cannot run {self.unifdef}: {error}") from error
        with open(name, "rb") as handle:
            return result.returncode, handle.read(), text(result.stderr).strip()

    def strip(self, path, side, data):
        for number, flags in enumerate(UNIFDEF_PASSES, 1):
            status, stripped, detail = self.run_pass(flags, data, retry=False)
            if status == 2:
                self.retries.append((path, side, number))
                status, stripped, detail = self.run_pass(flags, data, retry=True)
            if status not in (0, 1):
                raise MergeError(f"unifdef pass {number} exited with {status} on {side} {path}: "
                                 f"{detail}")
            data = stripped
        return data


def merge_file(scratch, ours, base, theirs):
    """(result bytes, conflict hunks) of git merge-file -p over the three texts."""
    names = []
    for label, data in zip(LABELS, (ours, base, theirs)):
        name = os.path.join(scratch, f"merge-{label}.txt")
        with open(name, "wb") as handle:
            handle.write(data)
        names.append(name)
    command = ["git", "merge-file", "-p", *(word for label in LABELS for word in ("-L", label)),
               *names]
    try:
        result = subprocess.run(command, capture_output=True, stdin=subprocess.DEVNULL,
                                env=dict(os.environ, **GIT_ENVIRONMENT), check=False)
    except OSError as error:
        raise MergeError(f"cannot run git: {error}") from error
    if not 0 <= result.returncode < 128:
        raise MergeError(f"git merge-file exited with {result.returncode}: "
                         f"{text(result.stderr).strip()}")
    hunks = sum(1 for line in result.stdout.split(b"\n") if line.rstrip(b"\r") == CONFLICT_START)
    return result.stdout, hunks


class PathResult:
    """What the helper does with one path that upstream changed."""

    def __init__(self, path, status, klass):
        self.path = path
        self.status = status          # A, M, D, or T (git diff --name-status)
        self.klass = klass            # upstream_paths class
        self.action = None            # keep-fork, keep-deleted, delete, strip-merge, git
        self.note = ""
        self.content = None           # strip-merge: the computed bytes
        self.mode = None
        self.stages = {}              # strip-merge with conflicts: {stage: (mode, bytes)}
        self.hunks = 0
        self.leftovers = []           # [(line, text)]
        self.fork_changed = False     # deleted upstream, changed in the fork since the base
        self.renamed_to = None
        self.commits = []             # fork-owned: ["<sha> <subject>"]

    @property
    def needs_hand_work(self):
        return bool(self.hunks or self.leftovers or self.commits or self.fork_changed)


def merged_mode(ours, base, theirs):
    if ours == base:
        return theirs
    return ours


def plan(repo, head, commit, stripper, blobs, scratch):
    """(merge base, [PathResult]) for merging commit into head, computed without changing the
    repository: the strip-merge results in memory, the other actions as names."""
    base = text(output(repo, "merge-base", head, commit)).strip()
    if not base:
        raise MergeError(f"{commit} and {head} have no merge base")
    trees = {name: tree(repo, sha) for name, sha in (("base", base), ("ours", head),
                                                     ("theirs", commit))}
    moved = None
    results = []
    for status, path in changed_paths(repo, base, commit):
        entry = PathResult(path, status, upstream_paths.classify(path))
        ours = trees["ours"].get(path)
        old = trees["base"].get(path)
        theirs = trees["theirs"].get(path)
        if entry.klass == REMOVED:
            entry.action = "keep-fork" if ours else "keep-deleted"
            entry.note = "added upstream; left out" if status == "A" else (
                "deleted upstream" if status == "D" else
                "changed upstream; deleted in the fork" if not ours else "changed upstream")
        elif entry.klass == FORK_OWNED:
            entry.action = "keep-fork" if ours else "keep-deleted"
            entry.note = "added upstream; left out" if status == "A" else (
                "deleted upstream; the fork keeps it" if status == "D" and ours else
                "changed upstream; deleted in the fork" if not ours else "changed upstream")
            log = output(repo, "log", "--format=%h %s", f"{base}..{commit}", "--", path)
            entry.commits = text(log).splitlines()
        elif status == "D" and not ours:
            entry.action = "keep-deleted" if entry.klass == STRIP else "git"
            entry.note = "deleted upstream and in the fork"
        elif status == "D":
            entry.action = "delete" if entry.klass == STRIP else "git"
            if ours[2] != old[2]:
                entry.fork_changed = True
                if moved is None:
                    moved = renames(repo, base, commit)
                entry.renamed_to = moved.get(path)
            if entry.klass == STRIP:
                entry.note = ("deleted upstream; changed in the fork since the merge base"
                              if entry.fork_changed else "deleted upstream")
                if entry.renamed_to:
                    entry.note += f"; upstream renamed it to {entry.renamed_to}"
            elif entry.fork_changed:
                # git's merge reports the modify/delete conflict; it is listed with the others.
                entry.fork_changed = False
        elif old and not ours:
            entry.action = "keep-deleted"
            entry.note = "changed upstream; deleted in the fork"
        elif entry.klass == STRIP and status in "AM" and theirs[0] in REGULAR_MODES \
                and (not ours or ours[0] in REGULAR_MODES) \
                and (not old or old[0] in REGULAR_MODES):
            entry.action = "strip-merge"
            upstream_side = stripper.strip(path, "upstream", blobs.read(theirs[2]))
            if not ours:
                entry.content, entry.mode = upstream_side, theirs[0]
                entry.note = "added upstream"
            else:
                base_side = stripper.strip(path, "base", blobs.read(old[2])) if old else b""
                fork_side = blobs.read(ours[2])
                entry.content, entry.hunks = merge_file(scratch, fork_side, base_side,
                                                        upstream_side)
                entry.mode = merged_mode(ours[0], old[0] if old else ours[0], theirs[0])
                entry.note = "added upstream and in the fork" if not old else "changed upstream"
                if entry.hunks:
                    if old:
                        entry.stages[1] = (old[0], base_side)
                    entry.stages[2] = (ours[0], fork_side)
                    entry.stages[3] = (theirs[0], upstream_side)
            entry.leftovers = upstream_paths.platform_directives(text(entry.content))
        else:
            entry.action = "git"
        results.append(entry)
    return base, results


def unmerged(repo):
    """The paths with unmerged index entries."""
    listing = output(repo, "ls-files", "-u", "-z")
    return sorted({text(entry.partition(b"\t")[2]) for entry in listing.split(b"\0") if entry})


def remove_file(repo, path):
    full = os.path.join(repo, path)
    if os.path.lexists(full):
        os.remove(full)
        parent = os.path.dirname(full)
        try:
            os.removedirs(parent)
        except OSError:
            pass


def write_file(repo, path, data, mode):
    full = os.path.join(repo, path)
    if os.path.lexists(full) and not os.path.isfile(full):
        raise MergeError(f"{path} in the work tree is not a regular file")
    os.makedirs(os.path.dirname(full) or repo, exist_ok=True)
    with open(full, "wb") as handle:
        handle.write(data)
    os.chmod(full, 0o755 if mode == "100755" else 0o644)


def pathspec(paths):
    return b"".join(path.encode("utf-8") + b"\0" for path in paths)


def apply(repo, results, auto_dir):
    """Write the planned results to the work tree and the index of repo, and copy every computed
    result to auto_dir."""
    restore = [entry.path for entry in results if entry.action == "keep-fork"]
    remove = [entry.path for entry in results if entry.action in ("keep-deleted", "delete")]
    if restore:
        git(repo, "checkout", "HEAD", "--pathspec-from-file=-", "--pathspec-file-nul",
            stdin=pathspec(restore))
    if remove:
        git(repo, "rm", "-q", "--cached", "--ignore-unmatch", "--pathspec-from-file=-",
            "--pathspec-file-nul", stdin=pathspec(remove))
        for path in remove:
            remove_file(repo, path)
    clean = []
    index_info = []
    for entry in results:
        if entry.action != "strip-merge":
            continue
        write_file(repo, entry.path, entry.content, entry.mode)
        copy = os.path.join(auto_dir, entry.path)
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        with open(copy, "wb") as handle:
            handle.write(entry.content)
        if not entry.stages:
            clean.append(entry.path)
            continue
        index_info.append(f"0 {'0' * 40}\t{entry.path}\n")
        for stage, (mode, data) in sorted(entry.stages.items()):
            obj = text(output(repo, "hash-object", "-w", "--no-filters", "--stdin",
                              stdin=data)).strip()
            index_info.append(f"{mode} {obj} {stage}\t{entry.path}\n")
    if clean:
        git(repo, "add", "--pathspec-from-file=-", "--pathspec-file-nul", stdin=pathspec(clean))
    if index_info:
        git(repo, "update-index", "--index-info", stdin="".join(index_info).encode("utf-8"))


def merge_messages(merge_output, paths):
    """{path: [git merge's CONFLICT lines that name path]}."""
    found = {path: [] for path in paths}
    for line in text(merge_output).splitlines():
        if line.startswith("CONFLICT"):
            for path in paths:
                if path in line:
                    found[path].append(line.strip())
    return found


def code(value):
    return "`" + value.replace("`", "'") + "`"


def report_lines(info, results, retries, other_conflicts, messages):
    by_class = {klass: [entry for entry in results if entry.klass == klass]
                for klass in (REMOVED, FORK_OWNED, STRIP, OTHER)}
    fork_deleted = [entry for entry in results if entry.klass in (STRIP, OTHER)
                    and entry.action == "keep-deleted" and entry.status != "D"]
    owned_changed = [entry for entry in by_class[FORK_OWNED] if entry.commits]
    merged = [entry for entry in results if entry.action == "strip-merge"]
    conflicted = [entry for entry in merged if entry.hunks]
    leftovers = [entry for entry in merged if entry.leftovers]
    deleted = [entry for entry in results if entry.klass == STRIP and entry.status == "D"]
    added = [entry for entry in results if entry.status == "A"]
    hand = info["hand_work"]
    lines = [
        f"# Upstream merge of {info['commit'][:8]} into {info['head'][:8]}", "",
        f"- Upstream commit: `{info['commit']}` {info['commit_subject']}",
        f"- Fork HEAD: `{info['head']}` {info['head_subject']}",
        f"- Merge base: `{info['base']}` {info['base_subject']}",
        f"- Upstream commits in the range: {info['commits']}",
        f"- git merge exit status: {info['merge_status']} "
        f"({len(info['git_conflicts'])} paths with conflicts before the helper)",
        "", "## Summary", "",
        "| Item | Count |", "|---|---|",
        f"| Paths changed upstream | {len(results)} |",
        f"| Removed areas (left out or kept deleted) | {len(by_class[REMOVED])} |",
        f"| Fork-deleted paths (kept deleted) | {len(fork_deleted)} |",
        f"| Fork-owned paths (fork side kept) | {len(by_class[FORK_OWNED])}, "
        f"{len(owned_changed)} with upstream commits to translate |",
        f"| Strip-merged paths | {len(merged)}, {len(conflicted)} with conflicts "
        f"({sum(entry.hunks for entry in conflicted)} hunks) |",
        f"| Leftover removed-platform directives | "
        f"{sum(len(entry.leftovers) for entry in leftovers)} in {len(leftovers)} files |",
        f"| unifdef -t retries | {len(retries)} |",
        f"| Deleted upstream (strip class) | {len(deleted)}, "
        f"{sum(1 for entry in deleted if entry.fork_changed)} changed in the fork |",
        f"| Other conflicts (git's merge) | {len(other_conflicts)} |",
        f"| Files added upstream | {len(added)} |",
        f"| Hand work remains | {'yes' if hand else 'no'} |",
        "", "## Removed areas", ""]
    lines += [f"- {code(entry.path)}: {entry.note}" for entry in by_class[REMOVED]] or ["None."]
    lines += ["", "## Fork-deleted paths", ""]
    lines += [f"- {code(entry.path)}: {entry.note}" for entry in fork_deleted] or ["None."]
    lines += ["", "## Fork-owned paths", "",
              "The fork's side is kept. Carry the intent of these upstream commits over by hand, "
              "or record why the fork does not need it.", ""]
    for entry in by_class[FORK_OWNED]:
        lines += [f"### {code(entry.path)}", "", f"{entry.note[:1].upper()}{entry.note[1:]}.", ""]
        lines += [f"- {code(commit.split(' ', 1)[0])} {commit.partition(' ')[2]}"
                  for commit in entry.commits]
        lines.append("")
    if not by_class[FORK_OWNED]:
        lines += ["None.", ""]
    lines += ["## Strip-merged paths", ""]
    if merged:
        lines += ["| Path | Upstream change | Conflict hunks | Leftover directives |",
                  "|---|---|---|---|"]
        lines += [f"| {code(entry.path)} | {entry.note} | {entry.hunks} | {len(entry.leftovers)} |"
                  for entry in merged]
    else:
        lines.append("None.")
    lines += ["", "## Leftover removed-platform directives", ""]
    lines += [f"- {code(f'{entry.path}:{number}')}: {code(directive)}"
              for entry in leftovers for number, directive in entry.leftovers] or ["None."]
    lines += ["", "## unifdef -t retries", ""]
    lines += [f"- {code(path)} ({side}, pass {number})" for path, side, number in retries] \
        or ["None."]
    lines += ["", "## Deleted upstream", ""]
    lines += [f"- {code(entry.path)}: {entry.note}" for entry in deleted] or ["None."]
    lines += ["", "## Other conflicts", ""]
    for path in other_conflicts:
        lines.append(f"- {code(path)}")
        lines += [f"  - {message}" for message in messages.get(path, [])]
    if not other_conflicts:
        lines.append("None.")
    lines += ["", "## Files added upstream", ""]
    actions = {"keep-fork": "fork side kept", "keep-deleted": "left out",
               "strip-merge": "stripped", "git": "taken as upstream has it",
               "delete": "deleted"}
    lines += [f"- {code(entry.path)} ({entry.klass}): {actions[entry.action]}"
              + (f", {entry.hunks} conflict hunks" if entry.hunks else "")
              for entry in added] or ["None."]
    return lines


def check_clean(repo):
    if rev(repo, "MERGE_HEAD"):
        raise MergeError(f"{repo} has a merge in progress (MERGE_HEAD exists)")
    status = output(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    if status.strip(b"\0"):
        first = text(status.split(b"\0")[0])
        raise MergeError(f"{repo} has uncommitted changes or untracked files ({first.strip()} "
                         "…); commit or remove them first")


def run(repo, commit, report, auto_dir, stripper_factory):
    """Merge commit in repo and resolve it; return (exit status, summary lines)."""
    repo = os.path.abspath(repo)
    top = rev(repo, "HEAD")
    if top is None:
        raise MergeError(f"{repo} is not a git work tree with a commit")
    sha = rev(repo, commit)
    if sha is None:
        raise MergeError(f"{commit} is not a commit of {repo}")
    check_clean(repo)
    if git(repo, "merge-base", "--is-ancestor", sha, top, codes=(0, 1))[0] == 0:
        raise MergeError(f"{sha[:8]} is already merged into HEAD")
    if os.path.lexists(auto_dir) and (not os.path.isdir(auto_dir) or os.listdir(auto_dir)):
        raise MergeError(f"--auto-dir {auto_dir} exists and is not an empty directory; remove "
                         "it or pick another one")
    with tempfile.TemporaryDirectory(prefix="platform-merge-") as scratch:
        stripper = stripper_factory(scratch)
        blobs = Blobs(repo)
        try:
            base, results = plan(repo, top, sha, stripper, blobs, scratch)
        finally:
            blobs.close()
        merge_status, merge_output = git(repo, "-c", "merge.renames=false", "merge", "--no-ff",
                                         "--no-commit", sha, codes=(0, 1))
        if not rev(repo, "MERGE_HEAD"):
            raise MergeError(f"git merge exited with {merge_status} and started no merge: "
                             f"{text(merge_output).strip()}")
        try:
            git_conflicts = unmerged(repo)
            os.makedirs(auto_dir, exist_ok=True)
            apply(repo, results, auto_dir)
            planned = {entry.path for entry in results if entry.stages}
            other_conflicts = [path for path in unmerged(repo) if path not in planned]
            messages = merge_messages(merge_output, other_conflicts)
            hand_work = any(entry.needs_hand_work for entry in results) or bool(other_conflicts)
            info = {"commit": sha, "head": top, "base": base, "merge_status": merge_status,
                    "git_conflicts": git_conflicts, "hand_work": hand_work,
                    "commit_subject": subject(repo, sha), "head_subject": subject(repo, top),
                    "base_subject": subject(repo, base),
                    "commits": int(text(output(repo, "rev-list", "--count", f"{base}..{sha}")))}
            lines = report_lines(info, results, stripper.retries, other_conflicts, messages)
            os.makedirs(os.path.dirname(os.path.abspath(report)), exist_ok=True)
            with open(report, "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
        except (OSError, MergeError) as error:
            raise MergeError(f"{error}; the merge is in progress, and git merge --abort drops "
                             "it") from error
    start = lines.index("## Summary")
    end = lines.index("## Removed areas")
    summary = [line for line in lines[start + 4:end] if line]
    return (1 if hand_work else 0), summary


def find_unifdef(name):
    found = shutil.which(name)
    if found is None:
        raise MergeError(f"unifdef not found ({name}); install it (Fedora: dnf install unifdef, "
                         "Ubuntu: apt install unifdef) or pass --unifdef PATH")
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Merge an upstream KytyPS5 commit with the removed platforms stripped from "
        "upstream's side.")
    parser.add_argument("--repo", required=True, help="the work tree to merge into")
    parser.add_argument("--report", required=True, help="the Markdown report to write")
    parser.add_argument("--auto-dir", required=True,
                        help="an empty or new directory for a copy of every computed result")
    parser.add_argument("--unifdef", default="unifdef", help="the unifdef program")
    parser.add_argument("commit", help="the upstream commit to merge")
    args = parser.parse_args(argv)
    try:
        unifdef = find_unifdef(args.unifdef)
        status, summary = run(args.repo, args.commit, args.report, args.auto_dir,
                              lambda scratch: UnifdefStripper(unifdef, scratch))
    except MergeError as error:
        print(f"{NAME}: error: {error}", file=sys.stderr)
        return 2
    for line in summary:
        print(f"{NAME}: {line.strip('|').replace(' | ', ': ').strip()}")
    print(f"{NAME}: report: {args.report}")
    print(f"{NAME}: {'hand work remains' if status else 'nothing left by hand'}; finish with "
          "git commit or drop the merge with git merge --abort")
    return status


if __name__ == "__main__":
    sys.exit(main())
