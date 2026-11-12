#!/usr/bin/env python3
"""Check the relative links and images of the Documentation_Set (task 29.6, Requirements 20.10
and 20.13).

The Documentation_Set is README.md, CONTRIBUTING.md, and every Markdown file under docs/, as
`git ls-files --cached --others --exclude-standard` lists them in the repository (tracked files
plus untracked files that are not ignored). In each file, outside fenced code blocks, inline code
spans, and HTML comments, the check reads:

  - the target of every Markdown link and image, [text](target) and ![alt](target), also when
    nested as in a linked badge, and of every reference definition, [label]: target;
  - every href and src attribute of an HTML tag.

A target with a URL scheme (https:, mailto:) or one that starts with // is external and is not
fetched. Every other target is relative. Its path, with backslash escapes and %-escapes decoded
and any ?query dropped, is resolved against the linking file's directory (a leading / against the
repository root) and must name a listed file, or a directory that holds one. The #fragment of a
link to a Markdown file (a bare #fragment names the linking file) must match an anchor of that
file: a heading anchor as GitHub generates it, or the id or name attribute of an HTML tag. The
fragments of links to other files (#L10 in a source file) are not checked.

GitHub's heading anchors (its github-slugger): take the text the heading shows (inline code keeps
its content, a link its text, an image and an HTML tag show nothing, emphasis markers go, and HTML
entities are decoded); lowercase it; drop every character that is not a letter, a mark, a decimal
or letter number, a connector punctuation such as "_", a space, or "-"; and replace each space with
"-". A heading whose anchor exists already in the file gets the first free "-1", "-2", ... suffix.
ATX headings (# Title) and setext headings (Title over === or ---) count.

Usage:
    check_links.py --repo DIR

It prints `<file>:<line>: <target>: <reason>` for each broken link or image, then one summary line,
`FAILED: docs_links: ...` or `PASS: docs_links: ...`.

Exit status: 0 when every relative link resolves, 1 when one does not, 2 on a usage or repository
error.
"""

import argparse
import bisect
import html
import os
import posixpath
import re
import stat
import subprocess
import sys
import unicodedata
from urllib.parse import unquote

NAME = "docs_links"
TOP_LEVEL_DOCUMENTS = ("README.md", "CONTRIBUTING.md")
DOCS_DIRECTORY = "docs/"
MARKDOWN_SUFFIXES = (".md", ".markdown")

FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
# An inline code span: a run of backticks, its content (not across a blank line), and the same run.
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)((?:(?!\n[ \t]*\n).)+?)(?<!`)\1(?!`)", re.S)
HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)
SCHEME = re.compile(r"^(?:[A-Za-z][A-Za-z0-9+.-]*:|//)")
# The target after "](": an <angle-bracketed> one, or one without spaces whose parentheses are
# balanced (one level deep). Finding every "](" also finds the image inside a linked badge.
INLINE_TARGET = re.compile(r"\]\(\s*(?:<([^<>\n]*)>|((?:[^\s()<>\\]|\\.|\([^\s()]*\))*))")
REFERENCE_DEFINITION = re.compile(r"^ {0,3}\[[^\]\n]+\]:[ \t]*(?:<([^<>\n]*)>|(\S+))", re.M)
HTML_TAG = re.compile(r"<[A-Za-z][^<>]*>")
TAG_LINK = re.compile(r"""(?<![\w-])(?:href|src)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.I)
TAG_ANCHOR = re.compile(r"""(?<![\w-])(?:id|name)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.I)
BACKSLASH_ESCAPE = re.compile(r"\\([!-/:-@\[-`{-~])")

ATX_HEADING = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
SETEXT_UNDERLINE = re.compile(r"^ {0,3}(?:=+|-+)[ \t]*$")
NOT_SETEXT_TEXT = re.compile(r"^\s*(?:[-*+]\s|\d+[.)]\s|#|>|\||<|```|~~~)")
IMAGE = re.compile(r"!\[(?:[^\[\]]|\[[^\[\]]*\])*\]\([^()]*(?:\([^()]*\)[^()]*)*\)")
LINK = re.compile(r"\[((?:[^\[\]]|\[[^\[\]]*\])*)\]\([^()]*(?:\([^()]*\)[^()]*)*\)")
# An emphasis "_" run: not between two letters or digits ("snake_case" keeps its "_").
UNDERSCORE_EMPHASIS = re.compile(r"(?<![^\W_])_+|_+(?![^\W_])")
ESCAPED_UNDERSCORE = "\0"
# The Unicode categories that github-slugger keeps (with " " and "-").
KEPT_CATEGORIES = {"Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "Pc"}


class RepoError(Exception):
    pass


def slug(text):
    """The GitHub anchor of a heading that shows text, before the duplicate suffix."""
    kept = []
    for character in text.lower():
        if character == " ":
            kept.append("-")
        elif character == "-" or unicodedata.category(character) in KEPT_CATEGORIES:
            kept.append(character)
    return "".join(kept)


class Slugger:
    """github-slugger's duplicate rule: the first free "<slug>-<n>"."""

    def __init__(self):
        self.occurrences = {}

    def slug(self, text):
        base = result = slug(text)
        while result in self.occurrences:
            self.occurrences[base] += 1
            result = f"{base}-{self.occurrences[base]}"
        self.occurrences[result] = 0
        return result


def heading_text(raw):
    """The text that the Markdown heading content raw shows."""
    shown = []
    position = 0
    for match in CODE_SPAN.finditer(raw):
        shown.append(inline_text(raw[position:match.start()]))
        content = match.group(2)
        if len(content) >= 2 and content[0] == " " and content[-1] == " " and content.strip():
            content = content[1:-1]
        shown.append(content)
        position = match.end()
    shown.append(inline_text(raw[position:]))
    return "".join(shown)


def inline_text(text):
    """The text that Markdown outside code spans shows: no images, links as their text, no HTML
    tags or emphasis markers, entities and backslash escapes decoded."""
    text = IMAGE.sub("", text)
    while True:
        reduced = LINK.sub(r"\1", text)
        if reduced == text:
            break
        text = reduced
    text = re.sub(r"</?[A-Za-z][^<>]*>", "", text)
    text = text.replace("\\_", ESCAPED_UNDERSCORE)
    text = UNDERSCORE_EMPHASIS.sub("", text)
    text = BACKSLASH_ESCAPE.sub(r"\1", text)
    text = html.unescape(text)
    return text.replace(ESCAPED_UNDERSCORE, "_")


def fenced_lines(lines):
    """The line numbers (1-based) of the fenced code blocks, fences included."""
    fenced = set()
    fence = None
    for number, line in enumerate(lines, 1):
        match = FENCE.match(line)
        if fence is None:
            if match and not (match.group(1)[0] == "`" and "`" in line[match.end():]):
                fence = match.group(1)
                fenced.add(number)
        else:
            fenced.add(number)
            if (match and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence)
                    and not line[match.end():].strip()):
                fence = None
    return fenced


def blank(text):
    """text with every character but a newline replaced by a space."""
    return re.sub(r"[^\n]", " ", text)


def masked_text(lines):
    """The lines joined, with fenced code blocks, HTML comments, and inline code spans blanked
    (offsets and line numbers kept)."""
    fenced = fenced_lines(lines)
    text = "\n".join(blank(line) if number in fenced else line
                     for number, line in enumerate(lines, 1))
    text = HTML_COMMENT.sub(lambda match: blank(match.group(0)), text)
    return CODE_SPAN.sub(lambda match: blank(match.group(0)), text)


def line_starts(text):
    return [0] + [match.end() for match in re.finditer("\n", text)]


def link_targets(lines):
    """(line number, target) of every link, image, reference definition, and HTML href or src."""
    text = masked_text(lines)
    starts = line_starts(text)
    found = []
    for pattern in (INLINE_TARGET, REFERENCE_DEFINITION):
        for match in pattern.finditer(text):
            group = 1 if match.group(1) is not None else 2
            found.append((match.start(group), match.group(group)))
    for tag in HTML_TAG.finditer(text):
        for match in TAG_LINK.finditer(tag.group(0)):
            group = 1 if match.group(1) is not None else 2
            found.append((tag.start() + match.start(group), match.group(group)))
    return [(bisect.bisect_right(starts, offset), target) for offset, target in sorted(found)]


def anchors(lines):
    """The anchors of a Markdown file: its GitHub heading anchors and its HTML id and name
    attributes."""
    fenced = fenced_lines(lines)
    text = masked_text(lines)
    masked_lines = text.split("\n")
    slugger = Slugger()
    found = set()
    for index, line in enumerate(lines):
        number = index + 1
        if number in fenced or not masked_lines[index].strip():
            continue
        match = ATX_HEADING.match(line)
        if match:
            found.add(slugger.slug(heading_text((match.group(2) or "").strip())))
            continue
        following = lines[index + 1] if index + 1 < len(lines) else ""
        if (number + 1 not in fenced and SETEXT_UNDERLINE.match(following)
                and not NOT_SETEXT_TEXT.match(line) and not ATX_HEADING.match(following)):
            previous = lines[index - 1] if index > 0 else ""
            if not previous.strip() or index == 0:
                found.add(slugger.slug(heading_text(line.strip())))
    for tag in HTML_TAG.finditer(text):
        for match in TAG_ANCHOR.finditer(tag.group(0)):
            found.add(match.group(1) if match.group(1) is not None else match.group(2))
    return found


def documentation_set(files):
    return sorted(path for path in files
                  if path in TOP_LEVEL_DOCUMENTS
                  or (path.startswith(DOCS_DIRECTORY) and path.endswith(".md")))


def read_lines(repo, path):
    """The lines of a regular text file, or None for anything else."""
    full = os.path.join(repo, path)
    try:
        if not stat.S_ISREG(os.lstat(full).st_mode):
            return None
        with open(full, "rb") as handle:
            data = handle.read()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise RepoError(f"cannot read {path}: {error}") from error
    return data.decode("utf-8", "replace").split("\n")


def resolve(source, target):
    """(path, fragment) of a relative target, or None for an external one. The path is relative to
    the repository root; it starts with "../" when the target leaves the repository."""
    target = BACKSLASH_ESCAPE.sub(r"\1", target.strip())
    if SCHEME.match(target):
        return None
    path, _, fragment = target.partition("#")
    path = unquote(path.split("?", 1)[0])
    if not path:
        return source, unquote(fragment)
    if path.startswith("/"):
        resolved = posixpath.normpath(path.lstrip("/") or ".")
    else:
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
    # "." is the repository root, which the directory set holds as "".
    return ("" if resolved == "." else resolved), unquote(fragment)


def check(repo, files):
    """(problems, files checked, relative links checked)."""
    listed = set(files)
    directories = {posixpath.dirname(path) for path in files}
    for directory in list(directories):
        while directory:
            directory = posixpath.dirname(directory)
            directories.add(directory)
    cache = {}

    def file_anchors(path):
        if path not in cache:
            lines = read_lines(repo, path)
            cache[path] = anchors(lines) if lines is not None else set()
        return cache[path]

    problems = []
    documents = documentation_set(files)
    relative = 0
    for source in documents:
        lines = read_lines(repo, source)
        if lines is None:
            continue
        for number, target in link_targets(lines):
            where = f"{source}:{number}: {target}"
            if not target.strip():
                problems.append(f"{where}: the link has no target")
                continue
            resolved = resolve(source, target)
            if resolved is None:
                continue
            relative += 1
            path, fragment = resolved
            if path == ".." or path.startswith("../"):
                problems.append(f"{where}: {path} is outside the repository")
                continue
            if path not in listed and path not in directories:
                problems.append(f"{where}: no file or directory {path} in the repository")
                continue
            if (fragment and path in listed and path.endswith(MARKDOWN_SUFFIXES)
                    and fragment not in file_anchors(path)):
                problems.append(f"{where}: {path} has no heading or HTML anchor #{fragment}")
    return problems, len(documents), relative


def list_files(repo):
    command = ["git", "-C", repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    try:
        result = subprocess.run(command, capture_output=True, check=False)
    except OSError as error:
        raise RepoError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise RepoError(f"git ls-files failed in {repo}: {message}")
    return sorted({name.decode("utf-8", "surrogateescape")
                   for name in result.stdout.split(b"\0") if name})


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check that every relative link and image in the Documentation_Set resolves "
        "to a file of the repository and every fragment to an anchor.")
    parser.add_argument("--repo", required=True, help="repository root (a Git work tree)")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.repo):
        print(f"error: {NAME}: {args.repo} is not a directory", file=sys.stderr)
        return 2
    try:
        problems, documents, relative = check(args.repo, list_files(args.repo))
    except RepoError as error:
        print(f"error: {NAME}: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(problem)
    if problems:
        print(f"FAILED: {NAME}: {len(problems)} broken link(s) in {documents} files "
              "(Requirements 20.10, 20.13)")
        return 1
    if not documents:
        print(f"FAILED: {NAME}: no Documentation_Set file in {args.repo}")
        return 1
    print(f"PASS: {NAME}: {relative} relative links and images in {documents} files resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
