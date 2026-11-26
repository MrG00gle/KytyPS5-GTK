"""Scratch git repositories for the tests of tools/upstream/.

Every git call of these tests and of the tools runs with HOME in the test's temporary directory,
without the user's and the system's git configuration (GIT_CONFIG_GLOBAL, GIT_CONFIG_NOSYSTEM),
and with a fixed test identity.
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

UPSTREAM_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Keep the local imports of the tests from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, UPSTREAM_DIR)

IDENTITY = {"GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.invalid",
            "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.invalid"}
NO_SIGNING = ("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
              "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false")

# A stand-in for unifdef with the options the helper passes: -m FILE, -t, -Usym, -Dsym=value. It
# drops "#ifdef sym" ... "#endif" blocks of the -U symbols, and exits 2 without -t when a line has
# a digit separator, as unifdef does on 1'000.
FAKE_UNIFDEF = """\
import re
import sys

arguments = sys.argv[1:]
name = arguments[-1]
with open(name, encoding="utf-8") as handle:
    text = handle.read()
if "-t" not in arguments and re.search(r"[0-9]'[0-9]", text):
    print(f"unifdef: {name}: 1: Unterminated char literal", file=sys.stderr)
    sys.exit(2)
undefined = {word[2:] for word in arguments if word.startswith("-U")}
kept = []
skipping = False
for line in text.splitlines(keepends=True):
    words = line.split()
    if len(words) == 2 and words[0] == "#ifdef" and words[1] in undefined:
        skipping = True
    elif skipping and words == ["#endif"]:
        skipping = False
    elif not skipping:
        kept.append(line)
result = "".join(kept)
with open(name, "w", encoding="utf-8") as handle:
    handle.write(result)
sys.exit(0 if result == text else 1)
"""

BLOCK = re.compile(r"^#ifdef _WIN32\n.*?^#endif\n", re.MULTILINE | re.DOTALL)


class FakeStripper:
    """Drops the "#ifdef _WIN32" ... "#endif" blocks; never retries."""

    def __init__(self):
        self.retries = []
        self.calls = []

    def strip(self, path, side, data):
        self.calls.append((path, side))
        return BLOCK.sub("", data.decode("utf-8")).encode("utf-8")


class ScratchRepository(unittest.TestCase):
    """A fork repository in a temporary directory, with helpers for commits and branches."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        home = os.path.join(self.temp.name, "home")
        os.mkdir(home)
        patcher = mock.patch.dict(os.environ, {"HOME": home, "GIT_CONFIG_GLOBAL": os.devnull,
                                               "GIT_CONFIG_NOSYSTEM": "1",
                                               "PYTHONDONTWRITEBYTECODE": "1", **IDENTITY})
        patcher.start()
        self.addCleanup(patcher.stop)
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                     "GIT_ALTERNATE_OBJECT_DIRECTORIES"):
            os.environ.pop(name, None)
        self.repo = self.new_repository("fork")
        self.report = os.path.join(self.temp.name, "out", "report.md")
        self.auto = os.path.join(self.temp.name, "out", "auto")

    def new_repository(self, name):
        path = os.path.join(self.temp.name, name)
        subprocess.run(["git", "init", "-q", "-b", "main", path], check=True, capture_output=True,
                       stdin=subprocess.DEVNULL)
        return path

    def git(self, *arguments, repo=None, check=True):
        result = subprocess.run(["git", *NO_SIGNING, "-C", repo or self.repo, *arguments],
                                check=check, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL)
        return result.stdout.strip()

    def write(self, files, repo=None):
        """Write the files ({path: text, or None to delete}) into the work tree."""
        for path, content in files.items():
            full = os.path.join(repo or self.repo, path)
            if content is None:
                if os.path.exists(full):
                    os.remove(full)
                continue
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8", newline="") as handle:
                handle.write(content)

    def commit(self, message, files=None, repo=None):
        """Commit the files with the message; return the SHA."""
        self.write(files or {}, repo)
        self.git("add", "-A", repo=repo)
        self.git("commit", "-q", "--allow-empty", "-m", message, repo=repo)
        return self.git("rev-parse", "HEAD", repo=repo)

    def trio(self, base, ours, theirs, upstream_commits=None):
        """Commit base on main, then theirs (or the upstream_commits, [(message, files)]) on the
        branch upstream, then ours on main. Return the SHA of the upstream branch."""
        self.commit("base", base)
        self.git("checkout", "-q", "-b", "upstream")
        for message, files in upstream_commits or [("upstream", theirs)]:
            self.commit(message, files)
        theirs_sha = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "main")
        self.commit("fork", ours)
        return theirs_sha

    def read(self, path):
        full = os.path.join(self.repo, path)
        if not os.path.exists(full):
            return None
        with open(full, encoding="utf-8", newline="") as handle:
            return handle.read()

    def staged(self, path):
        """The stage-0 index content of path, or None."""
        result = subprocess.run(["git", "-C", self.repo, "show", f":0:{path}"],
                                capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                check=False)
        return result.stdout if result.returncode == 0 else None

    def unmerged(self):
        """{path: [stages]} of the unmerged index entries."""
        found = {}
        for line in self.git("ls-files", "-u").splitlines():
            info, _, path = line.partition("\t")
            found.setdefault(path, []).append(int(info.split()[2]))
        return found

    def fake_unifdef(self):
        """The path of an executable stand-in for unifdef (FAKE_UNIFDEF)."""
        path = os.path.join(self.temp.name, "bin", "unifdef")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(f"#!{sys.executable}\n" + FAKE_UNIFDEF)
        os.chmod(path, 0o755)
        return path

    def report_text(self):
        with open(self.report, encoding="utf-8") as handle:
            return handle.read()

    def section(self, heading):
        """The lines of the report's section "## heading", without blank lines."""
        text = self.report_text().split(f"\n## {heading}\n", 1)[1].split("\n## ", 1)[0]
        return [line for line in text.splitlines() if line]
