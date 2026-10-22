#!/usr/bin/env python3
"""Capture and compare the Linux preprocessed output of platform-conditional files.

Requirements 3.3 and 4.3: removing the Windows and Apple preprocessor branches must
leave the Linux x86-64 preprocessed output of every affected first-party file
unchanged except for line markers.

The script uses compile_commands.json from a Main_Branch build (configure with
CMAKE_EXPORT_COMPILE_COMMANDS=ON and build it first, so generated headers exist).

  capture  Preprocesses every Git-tracked file under src/ and tests/ that mentions a
           targeted macro (_WIN32, WIN32, KYTY_PLATFORM_WINDOWS, _MSC_VER,
           __MINGW32__, __MINGW64__, __APPLE__, KYTY_LINKED_GUEST_ADDRESS_SPACE) and
           writes the reference outputs and a manifest into the build directory
           (default: <build-dir>/preprocessed-reference).

  compare  Preprocesses the same files from another checkout (default: the checkout
           that contains this script) with the same Main_Branch compiler flags and
           reports every difference from the reference outputs other than line
           markers. Exits 1 when there is any difference or error.

How a file is preprocessed:
  * A source file is preprocessed once for every distinct compile command that
    compiles it (one per CMake target, identical commands merged).
  * A header or .inc file is preprocessed through a translation unit that includes
    it, using that translation unit's flags (found with -M at capture time and
    recorded in the manifest). Translation units that include the file directly are
    preferred, then those that mention no targeted macro, so the chosen one is unlikely
    to be deleted by the platform removal. Capture prints every choice.
  * The output of a file is the set of preprocessed lines that the line markers
    attribute to that file. Line markers themselves, and the blank lines the
    preprocessor emits to keep line positions, are not compared. As with any -E
    output, macro definitions are not part of it; their expansions are.
  * __LINE__, __DATE__, __TIME__, and __TIMESTAMP__ are defined to themselves, and
    -fmacro-prefix-map strips the checkout and build directory prefixes from
    __FILE__, so line positions and checkout locations do not show up as
    differences.
  * In compare mode only src/ and tests/ paths are redirected to the compared
    checkout. Third-party headers and generated headers still come from the
    Main_Branch source and build directories.

Files that no longer exist in the compared checkout are listed and not compared
(Requirement 4.3 covers the affected files that remain in the Repository). Files
that the Main_Branch build never compiles on Linux have no Linux preprocessed
output; they are listed in both modes.

Usage:
  compare_preprocessed.py capture --build-dir MAIN_BUILD [--reference-dir DIR] [--jobs N]
  compare_preprocessed.py compare --build-dir MAIN_BUILD [--source-dir CHECKOUT]
                                  [--reference-dir DIR] [--jobs N]
"""

import argparse
import concurrent.futures
import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

TARGET_MACROS = (
    "_WIN32",
    "WIN32",
    "KYTY_PLATFORM_WINDOWS",
    "_MSC_VER",
    "__MINGW32__",
    "__MINGW64__",
    "__APPLE__",
    "KYTY_LINKED_GUEST_ADDRESS_SPACE",
)
MACRO_RE = re.compile(r"(?<![A-Za-z0-9_])(?:" + "|".join(map(re.escape, TARGET_MACROS)) +
                      r")(?![A-Za-z0-9_])")
FIRST_PARTY_DIRS = ("src", "tests")
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
HEADER_EXTS = {".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp"}
# Builtin macros whose expansion depends on the line position or the clock.
NORMALIZE_FLAGS = [
    "-Wno-builtin-macro-redefined",
    "-D__LINE__=__LINE__",
    "-D__DATE__=__DATE__",
    "-D__TIME__=__TIME__",
    "-D__TIMESTAMP__=__TIMESTAMP__",
]
# Compile command options (with their value) that do not affect preprocessing.
DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ"}
DROP_ALONE = {"-c", "-M", "-MM", "-MD", "-MMD", "-MP", "-fcolor-diagnostics",
              "-fno-color-diagnostics", "-fdiagnostics-color"}
LINE_MARKER_RE = re.compile(r'^#\s*(?:line\s+)?\d+\s+"((?:[^"\\]|\\.)*)"')
MANIFEST_NAME = "manifest.json"
OUTPUTS_DIR = "outputs"
MANIFEST_VERSION = 1

SCRIPT_CHECKOUT = Path(__file__).resolve().parent.parent.parent


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def read_cmake_home(build_dir):
    cache = build_dir / "CMakeCache.txt"
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                return Path(line.split("=", 1)[1]).resolve()
    except OSError as error:
        fail(f"cannot read {cache}: {error}")
    fail(f"CMAKE_HOME_DIRECTORY is not set in {cache}")


def list_first_party(root):
    """Return the Git-tracked (or, outside Git, existing) C-family files under src and tests."""
    try:
        result = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", *FIRST_PARTY_DIRS],
                                check=True, capture_output=True)
        paths = [p for p in result.stdout.decode("utf-8").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        paths = []
        for top in FIRST_PARTY_DIRS:
            for dirpath, _, filenames in os.walk(root / top):
                for name in filenames:
                    paths.append(os.path.relpath(os.path.join(dirpath, name), root))
    exts = SOURCE_EXTS | HEADER_EXTS
    return sorted(p.replace(os.sep, "/") for p in paths if Path(p).suffix.lower() in exts)


def mentions_target_macro(path):
    try:
        return MACRO_RE.search(path.read_text(encoding="utf-8", errors="replace")) is not None
    except OSError:
        return False


def entry_arguments(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def strip_compile_args(args, source_file, directory):
    """Drop the output, dependency, and source file arguments of a compile command."""
    result = []
    source_real = os.path.realpath(os.path.join(directory, source_file))
    skip = False
    for index, arg in enumerate(args):
        if skip:
            skip = False
            continue
        if index == 0:
            result.append(arg)
            continue
        if arg in DROP_WITH_VALUE:
            skip = True
            continue
        if arg in DROP_ALONE or arg.startswith("-fdiagnostics-color="):
            continue
        if any(arg.startswith(opt) and len(arg) > len(opt) for opt in ("-MF", "-MT", "-MQ")):
            continue
        if not arg.startswith("-") and os.path.realpath(os.path.join(directory, arg)) == source_real:
            continue
        result.append(arg)
    return result


def target_name(entry, fallback):
    match = re.search(r"CMakeFiles/([^/]+)\.dir/", entry.get("output", "") or entry.get("command", ""))
    return match.group(1) if match else fallback


def load_compile_commands(build_dir, main_root):
    """Map each first-party relative path to its distinct compile variants."""
    path = build_dir / "compile_commands.json"
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read {path}: {error}")
    variants = {}
    for number, entry in enumerate(entries):
        directory = entry["directory"]
        real = Path(os.path.realpath(os.path.join(directory, entry["file"])))
        try:
            rel = real.relative_to(main_root).as_posix()
        except ValueError:
            continue
        if not rel.startswith(tuple(d + "/" for d in FIRST_PARTY_DIRS)):
            continue
        args = strip_compile_args(entry_arguments(entry), entry["file"], directory)
        file_variants = variants.setdefault(rel, [])
        name = target_name(entry, f"cmd{number}")
        for variant in file_variants:
            if variant["args"] == args and variant["directory"] == directory:
                variant["targets"].append(name)
                break
        else:
            file_variants.append({"targets": [name], "args": args, "directory": directory})
    for file_variants in variants.values():
        used = set()
        for variant in file_variants:
            base = re.sub(r"[^A-Za-z0-9_.-]", "_", variant["targets"][0])
            name, counter = base, 1
            while name in used:
                counter += 1
                name = f"{base}-{counter}"
            used.add(name)
            variant["name"] = name
    return variants


def remap_arg(arg, main_root, source_root):
    """Point src/ and tests/ paths of the Main_Branch checkout at the compared checkout."""
    if main_root == source_root:
        return arg
    pattern = re.escape(str(main_root)) + r"/(" + "|".join(FIRST_PARTY_DIRS) + r")(?=/|$)"
    return re.sub(pattern, lambda m: f"{source_root}/{m.group(1)}", arg)


def prefix_map_flags(roots):
    flags = []
    for root in sorted({str(r) for r in roots}, key=len, reverse=True):
        flags.append(f"-fmacro-prefix-map={root}/=")
    return flags


def build_command(variant, file_path, extra, main_root, source_root, build_dir):
    args = [remap_arg(a, main_root, source_root) for a in variant["args"]]
    roots = {main_root, source_root, build_dir}
    return args + NORMALIZE_FLAGS + prefix_map_flags(roots) + extra + [str(file_path)]


def run(command, cwd):
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True)
    except OSError as error:
        return 127, b"", str(error).encode()
    return result.returncode, result.stdout, result.stderr


class PathResolver:
    def __init__(self):
        self.cache = {}

    def resolve(self, name, cwd):
        key = (name, cwd)
        if key not in self.cache:
            if name.startswith("<") and name.endswith(">"):
                self.cache[key] = None
            else:
                self.cache[key] = os.path.realpath(os.path.join(cwd, name))
        return self.cache[key]


def unescape_marker_name(name):
    return re.sub(r"\\(.)", r"\1", name)


def attributed_lines(output, target, cwd, resolver):
    """Return the non-blank preprocessed lines that line markers attribute to target.

    The second value tells whether any line marker named target, so a file that
    was included but produced no lines can be told apart from one never included.
    """
    lines = []
    current = None
    seen = False
    for line in output.decode("utf-8", errors="surrogateescape").split("\n"):
        match = LINE_MARKER_RE.match(line)
        if match:
            current = resolver.resolve(unescape_marker_name(match.group(1)), cwd)
            seen = seen or current == target
            continue
        if current == target and line.strip():
            lines.append(line.rstrip("\r"))
    return lines, seen


def parse_make_deps(output, cwd):
    text = output.decode("utf-8", errors="surrogateescape").replace("\\\n", " ")
    _, _, deps = text.partition(":")
    names = re.findall(r"(?:\\ |[^\s])+", deps)
    return {os.path.realpath(os.path.join(cwd, n.replace("\\ ", " "))) for n in names}


def preprocess(job, root_main, root_source, build_dir, resolver):
    """Run one preprocessing job and return (status, lines, message)."""
    file_path = root_source / job["file"]
    tu_path = root_source / job["tu"]
    if not tu_path.exists():
        return "error", [], f"translation unit {job['tu']} no longer exists"
    command = build_command(job["variant"], tu_path, [], root_main, root_source, build_dir)
    cwd = job["variant"]["directory"]
    code, out, err = run(command + ["-E"], cwd)
    if code != 0:
        return "error", [], f"preprocessor exit {code}: " + err.decode("utf-8", "replace").strip()
    lines, seen = attributed_lines(out, os.path.realpath(file_path), cwd, resolver)
    if not seen:
        return "error", [], f"{job['tu']} does not include {job['file']}"
    return "ok", lines, ""


def find_including_tu(header, candidates, variants, main_root, build_dir, deps_cache, jobs):
    """Return the first candidate translation unit whose -M dependencies include header."""
    header_real = os.path.realpath(main_root / header)

    def deps_of(tu):
        if tu not in deps_cache:
            variant = variants[tu][0]
            command = build_command(variant, main_root / tu, [], main_root, main_root, build_dir)
            code, out, _ = run(command + ["-M"], variant["directory"])
            deps_cache[tu] = parse_make_deps(out, variant["directory"]) if code == 0 else set()
        return deps_cache[tu]

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for start in range(0, len(candidates), jobs):
            batch = candidates[start:start + jobs]
            for tu, deps in zip(batch, pool.map(deps_of, batch)):
                if header_real in deps:
                    return tu
    return None


def rank_candidates(header, variants, main_root):
    """Order translation units: direct includers first, then those without a targeted macro."""
    base = Path(header).name
    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]*)[>"]', re.M)
    ranked = []
    for tu in sorted(variants):
        if Path(tu).suffix.lower() not in SOURCE_EXTS:
            continue
        try:
            text = (main_root / tu).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        direct = any(Path(inc).name == base for inc in include_re.findall(text))
        ranked.append((not direct, MACRO_RE.search(text) is not None, tu))
    ranked.sort()
    return [tu for _, _, tu in ranked]


def output_path(reference_dir, rel, variant_name):
    return reference_dir / OUTPUTS_DIR / rel / f"{variant_name}.i"


def capture(args):
    build_dir = args.build_dir.resolve()
    main_root = read_cmake_home(build_dir)
    reference_dir = (args.reference_dir or build_dir / "preprocessed-reference").resolve()
    variants = load_compile_commands(build_dir, main_root)
    files = [rel for rel in list_first_party(main_root) if mentions_target_macro(main_root / rel)]
    print(f"{len(files)} first-party files under src/ and tests/ mention a targeted macro")

    jobs = []
    not_compiled = []
    deps_cache = {}
    for rel in files:
        if rel in variants:
            for variant in variants[rel]:
                jobs.append({"file": rel, "tu": rel, "variant": variant})
            continue
        if Path(rel).suffix.lower() in HEADER_EXTS:
            candidates = rank_candidates(rel, variants, main_root)
            tu = find_including_tu(rel, candidates, variants, main_root, build_dir, deps_cache,
                                   args.jobs)
            if tu is not None:
                print(f"{rel}: preprocessed through {tu} [{variants[tu][0]['name']}]")
                jobs.append({"file": rel, "tu": tu, "variant": variants[tu][0]})
                continue
        not_compiled.append(rel)

    if (reference_dir / MANIFEST_NAME).exists():
        shutil.rmtree(reference_dir / OUTPUTS_DIR, ignore_errors=True)
    reference_dir.mkdir(parents=True, exist_ok=True)

    resolver = PathResolver()
    errors = 0
    entries = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = pool.map(lambda job: preprocess(job, main_root, main_root, build_dir, resolver),
                           jobs)
        for job, (status, lines, message) in zip(jobs, results):
            out = output_path(reference_dir, job["file"], job["variant"]["name"])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text("".join(line + "\n" for line in lines), encoding="utf-8",
                           errors="surrogateescape")
            if status != "ok":
                errors += 1
                print(f"ERROR {job['file']} [{job['variant']['name']}]: {message}")
            entries.append({
                "file": job["file"],
                "tu": job["tu"],
                "variant": job["variant"],
                "status": status,
                "message": message,
                "lines": len(lines),
            })

    manifest = {
        "version": MANIFEST_VERSION,
        "main_source_root": str(main_root),
        "build_dir": str(build_dir),
        "target_macros": list(TARGET_MACROS),
        "files": files,
        "not_compiled_on_linux": not_compiled,
        "entries": entries,
    }
    (reference_dir / MANIFEST_NAME).write_text(json.dumps(manifest, indent=1) + "\n",
                                               encoding="utf-8")
    via_tu = sum(1 for e in entries if e["tu"] != e["file"])
    print(f"captured {len(entries)} outputs ({via_tu} through an including translation unit) "
          f"into {reference_dir}")
    for rel in not_compiled:
        print(f"not compiled on Linux (no compile command and no including translation unit): {rel}")
    if errors:
        print(f"{errors} outputs could not be captured")
        return 1
    return 0


def compare(args):
    build_dir = args.build_dir.resolve()
    reference_dir = (args.reference_dir or build_dir / "preprocessed-reference").resolve()
    source_root = (args.source_dir or SCRIPT_CHECKOUT).resolve()
    try:
        manifest = json.loads((reference_dir / MANIFEST_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read the reference manifest in {reference_dir}: {error} "
             "(run the capture mode first)")
    if manifest.get("version") != MANIFEST_VERSION:
        fail(f"unsupported manifest version {manifest.get('version')}")
    main_root = Path(manifest["main_source_root"])
    if Path(manifest["build_dir"]) != build_dir:
        fail(f"the reference was captured from {manifest['build_dir']}, not {build_dir}")

    jobs = []
    deleted = []
    for entry in manifest["entries"]:
        if not (source_root / entry["file"]).exists():
            if entry["file"] not in deleted:
                deleted.append(entry["file"])
            continue
        jobs.append(entry)

    resolver = PathResolver()
    differences = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = pool.map(lambda job: preprocess(job, main_root, source_root, build_dir, resolver),
                           jobs)
        for job, (status, lines, message) in zip(jobs, results):
            label = f"{job['file']} [{job['variant']['name']}]"
            if job["tu"] != job["file"]:
                label += f" via {job['tu']}"
            if job["status"] != "ok":
                differences += 1
                print(f"DIFFERENCE {label}: the reference capture failed: {job['message']}")
                continue
            if status != "ok":
                differences += 1
                print(f"DIFFERENCE {label}: {message}")
                continue
            reference = output_path(reference_dir, job["file"], job["variant"]["name"])
            expected = reference.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
            if expected != lines:
                differences += 1
                print(f"DIFFERENCE {label}:")
                for diff_line in difflib.unified_diff(expected, lines, f"main/{job['file']}",
                                                      f"current/{job['file']}", lineterm=""):
                    print(diff_line)

    for rel in deleted:
        print(f"not compared (the file no longer exists): {rel}")
    for rel in manifest.get("not_compiled_on_linux", []):
        print(f"not compared (not compiled on Linux at the Main_Branch reference): {rel}")
    print(f"compared {len(jobs)} outputs of {len({j['file'] for j in jobs})} files from "
          f"{source_root}: {differences} with differences other than line markers")
    return 1 if differences else 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Capture or compare the Linux preprocessed output of the first-party files "
        "that mention a Windows or Apple platform macro.")
    sub = parser.add_subparsers(dest="mode", required=True)
    for mode in ("capture", "compare"):
        cmd = sub.add_parser(mode)
        cmd.add_argument("--build-dir", type=Path, required=True,
                         help="Main_Branch build directory that holds compile_commands.json")
        cmd.add_argument("--reference-dir", type=Path,
                         help="reference output directory (default: "
                         "<build-dir>/preprocessed-reference)")
        cmd.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                         help="parallel preprocessor runs (default: %(default)s)")
        if mode == "compare":
            cmd.add_argument("--source-dir", type=Path,
                             help=f"checkout to compare (default: {SCRIPT_CHECKOUT})")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        fail("--jobs must be at least 1")
    return capture(args) if args.mode == "capture" else compare(args)


if __name__ == "__main__":
    sys.exit(main())
