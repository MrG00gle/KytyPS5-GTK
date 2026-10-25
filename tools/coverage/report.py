#!/usr/bin/env python3
"""Write the coverage summary and the HTML index from an llvm-cov export.

Requirements 17.2 and 17.3: the report lists covered count, total count, and
a percentage rounded to two decimals for lines, functions, and branches, for
every Coverage_Scope file, for every directory under src that holds a scope
file (aggregated recursively), and for the whole scope. Scope files that no
binary contains get a 0/0 entry; files outside the scope are not listed.

Coverage_Scope is `git ls-files src` filtered to C and C++ source
extensions. Groups (groups.json) aggregate the files under path prefixes for
the thresholds of Requirements 18.1 to 18.3.

Usage:
    report.py --export EXPORT_JSON --source-dir REPO --groups GROUPS_JSON
              --output-dir COVERAGE_DIR [--html-files-dir DIR]

EXPORT_JSON is written by `llvm-cov export -format=text -summary-only`.
The tool writes COVERAGE_DIR/summary.json and COVERAGE_DIR/html/index.html
from the same rounded values. Each file row links to the page that
`llvm-cov show -format=html -output-dir=DIR` wrote for it
(DIR/coverage/<absolute path>.html, DIR defaults to COVERAGE_DIR/html/files)
when that page exists.

Exit status: 0 on success, 2 on a usage or input error.
"""

import argparse
import html
import json
import os
import subprocess
import sys
import urllib.parse
from decimal import Decimal, ROUND_HALF_UP

SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                     ".inc")
METRICS = ("lines", "functions", "branches")
TWO_PLACES = Decimal("0.01")


class InputError(Exception):
    """An input file or the source tree cannot be used."""


def percent(covered, total):
    """Return covered * 100 / total rounded half up to two decimals.

    A total of 0 gives 0.00, as llvm-cov reports it.
    """
    if total == 0:
        return Decimal("0.00")
    value = Decimal(covered) * 100 / Decimal(total)
    return value.quantize(TWO_PLACES, rounding=ROUND_HALF_UP)


def is_source_file(path):
    return path.endswith(SOURCE_EXTENSIONS)


def scope_files(source_dir):
    """Return the sorted Coverage_Scope paths (relative, '/' separated)."""
    try:
        result = subprocess.run(
            ["git", "-C", source_dir, "ls-files", "-z", "--", "src"],
            check=False, capture_output=True)
    except OSError as error:
        raise InputError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise InputError(f"git ls-files failed in {source_dir}: {message}")
    names = result.stdout.decode("utf-8", "surrogateescape").split("\0")
    return sorted(name for name in names if name and is_source_file(name))


def relative_path(filename, source_dir):
    """Return filename relative to source_dir, or None when it is outside."""
    if not os.path.isabs(filename):
        filename = os.path.join(source_dir, filename)
    pairs = ((os.path.normpath(filename), os.path.normpath(os.path.abspath(source_dir))),
             (os.path.realpath(filename), os.path.realpath(source_dir)))
    for path, root in pairs:
        rel = os.path.relpath(path, root)
        if rel != os.curdir and rel != os.pardir and not rel.startswith(os.pardir + os.sep):
            return rel.replace(os.sep, "/")
    return None


def _count(summary, metric, key):
    value = summary.get(metric, {}).get(key, 0)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise InputError(f"invalid {metric} {key} value: {value!r}")
    return value


def export_file_counts(export, source_dir):
    """Map relative paths to {metric: (covered, total)} from an export.

    The first entry for a path wins; llvm-cov writes each file once.
    """
    if not isinstance(export, dict) or not isinstance(export.get("data"), list):
        raise InputError("export has no 'data' list")
    counts = {}
    for data in export["data"]:
        files = data.get("files", []) if isinstance(data, dict) else None
        if not isinstance(files, list):
            raise InputError("export data entry has no 'files' list")
        for entry in files:
            if not isinstance(entry, dict) or not isinstance(entry.get("filename"), str) \
                    or not isinstance(entry.get("summary"), dict):
                raise InputError("export file entry lacks 'filename' or 'summary'")
            rel = relative_path(entry["filename"], source_dir)
            if rel is None or rel in counts:
                continue
            summary = entry["summary"]
            counts[rel] = {metric: (_count(summary, metric, "covered"),
                                    _count(summary, metric, "count"))
                           for metric in METRICS}
    return counts


def _metrics(covered_total):
    return {metric: {"covered": covered, "total": total,
                     "percent": percent(covered, total)}
            for metric, (covered, total) in covered_total.items()}


def _add(target, counts):
    for metric in METRICS:
        covered, total = counts[metric]
        old_covered, old_total = target[metric]
        target[metric] = (old_covered + covered, old_total + total)


def _empty():
    return {metric: (0, 0) for metric in METRICS}


def build_summary(export_files, scope_paths, groups):
    """Build the summary.json structure.

    export_files: {relative path: {metric: (covered, total)}};
    scope_paths: the Coverage_Scope paths; groups: {name: [path prefixes]}.
    Files outside scope_paths are dropped; scope files missing from
    export_files count as 0/0.
    """
    files = {}
    directories = {}
    scope = _empty()
    group_totals = {name: _empty() for name in groups}
    for path in sorted(set(scope_paths)):
        counts = export_files.get(path, _empty())
        files[path] = counts
        _add(scope, counts)
        parts = path.split("/")
        for depth in range(2, len(parts)):
            directory = "/".join(parts[:depth])
            _add(directories.setdefault(directory, _empty()), counts)
        for name, prefixes in groups.items():
            if any(path.startswith(prefix) for prefix in prefixes):
                _add(group_totals[name], counts)
    return {
        "scope": _metrics(scope),
        "directories": {name: _metrics(directories[name]) for name in sorted(directories)},
        "files": {name: _metrics(files[name]) for name in files},
        "groups": {name: _metrics(group_totals[name]) for name in groups},
    }


def dump_json(value, indent=0):
    """Serialize value as JSON, writing Decimal percents with two decimals."""
    pad = "  " * (indent + 1)
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f"{pad}{json.dumps(key)}: {dump_json(item, indent + 1)}"
                 for key, item in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + "  " * indent + "}"
    if isinstance(value, Decimal):
        return f"{value:.2f}"
    return json.dumps(value)


def load_groups(path):
    """Return {name: [prefixes]} and {name: {metric: threshold}} from groups.json."""
    data = load_json(path, "groups file")
    groups = data.get("groups") if isinstance(data, dict) else None
    if not isinstance(groups, dict):
        raise InputError(f"groups file {path} has no 'groups' object")
    prefixes = {}
    thresholds = {}
    for name, group in groups.items():
        paths = group.get("paths") if isinstance(group, dict) else None
        limits = group.get("thresholds", {}) if isinstance(group, dict) else None
        if not isinstance(paths, list) or not paths \
                or not all(isinstance(item, str) and item for item in paths):
            raise InputError(f"group {name} in {path} needs a non-empty 'paths' list")
        if not isinstance(limits, dict) or not all(
                metric in METRICS and isinstance(limit, (int, float))
                and not isinstance(limit, bool) and 0 <= limit <= 100
                for metric, limit in limits.items()):
            raise InputError(f"group {name} in {path} has invalid 'thresholds'")
        prefixes[name] = list(paths)
        thresholds[name] = {metric: Decimal(str(limit)) for metric, limit in limits.items()}
    return prefixes, thresholds


def load_json(path, what):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise InputError(f"cannot read {what} {path}: {error}") from error
    except ValueError as error:
        raise InputError(f"{what} {path} is not valid JSON: {error}") from error


def _cells(metrics):
    cells = []
    for metric in METRICS:
        values = metrics[metric]
        cells.extend((str(values["covered"]), str(values["total"]), f"{values['percent']:.2f}"))
    return "".join(f"<td>{cell}</td>" for cell in cells)


def _table(caption, first_column, rows):
    header = "".join(
        f'<th scope="col">{html.escape(metric.capitalize())} {label}</th>'
        for metric in METRICS for label in ("covered", "total", "%"))
    lines = ["<table>", f"<caption>{html.escape(caption)}</caption>",
             f'<thead><tr><th scope="col">{html.escape(first_column)}</th>{header}</tr></thead>',
             "<tbody>"]
    for name_html, metrics in rows:
        lines.append(f'<tr><th scope="row">{name_html}</th>{_cells(metrics)}</tr>')
    lines.extend(("</tbody>", "</table>"))
    return "\n".join(lines)


def file_page(path, source_dir, html_files_dir):
    """Return the llvm-cov show page for a scope file, or None when absent."""
    roots = (os.path.normpath(os.path.abspath(source_dir)), os.path.realpath(source_dir))
    for root in roots:
        absolute = os.path.join(root, path)
        page = os.path.join(html_files_dir, "coverage", absolute.lstrip("/") + ".html")
        if os.path.isfile(page):
            return page
    return None


def render_html(summary, links):
    """Render index.html; links maps a file path to its href (or nothing)."""
    def file_name(path):
        href = links.get(path)
        if href is None:
            return html.escape(path)
        return f'<a href="{html.escape(href, quote=True)}">{html.escape(path)}</a>'

    sections = [
        _table("Coverage_Scope total", "Scope", [("src", summary["scope"])]),
        _table("Groups", "Group",
               [(html.escape(name), metrics) for name, metrics in summary["groups"].items()]),
        _table("Directories under src (recursive)", "Directory",
               [(html.escape(name), metrics) for name, metrics in summary["directories"].items()]),
        _table("Files", "File",
               [(file_name(name), metrics) for name, metrics in summary["files"].items()]),
    ]
    return "\n".join([
        "<!DOCTYPE html>",
        '<html lang="en">',
        "<head>",
        '<meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width, initial-scale=1">',
        "<title>Coverage report</title>",
        "<style>table{border-collapse:collapse;margin-bottom:2em}"
        "th,td{border:1px solid #555;padding:0.2em 0.5em;text-align:right}"
        "th[scope=row]{text-align:left}caption{text-align:left;font-weight:bold}</style>",
        "</head>",
        "<body>",
        "<main>",
        "<h1>Coverage report</h1>",
        *sections,
        "</main>",
        "</body>",
        "</html>",
        "",
    ])


def write_report(summary, output_dir, source_dir, html_files_dir):
    html_dir = os.path.join(output_dir, "html")
    os.makedirs(html_dir, exist_ok=True)
    links = {}
    for path in summary["files"]:
        page = file_page(path, source_dir, html_files_dir)
        if page is not None:
            links[path] = urllib.parse.quote(os.path.relpath(page, html_dir))
    with open(os.path.join(output_dir, "summary.json"), "w", encoding="utf-8") as handle:
        handle.write(dump_json(summary) + "\n")
    with open(os.path.join(html_dir, "index.html"), "w", encoding="utf-8") as handle:
        handle.write(render_html(summary, links))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write coverage/summary.json and coverage/html/index.html "
        "from an llvm-cov export.")
    parser.add_argument("--export", required=True,
                        help="llvm-cov export -format=text -summary-only output")
    parser.add_argument("--source-dir", required=True, help="repository checkout")
    parser.add_argument("--groups", required=True, help="groups.json")
    parser.add_argument("--output-dir", required=True, help="coverage directory")
    parser.add_argument("--html-files-dir",
                        help="llvm-cov show -format=html output directory "
                        "(default: OUTPUT_DIR/html/files)")
    args = parser.parse_args(argv)
    html_files_dir = args.html_files_dir or os.path.join(args.output_dir, "html", "files")

    try:
        prefixes, _ = load_groups(args.groups)
        export = load_json(args.export, "export")
        counts = export_file_counts(export, args.source_dir)
        summary = build_summary(counts, scope_files(args.source_dir), prefixes)
        write_report(summary, args.output_dir, args.source_dir, html_files_dir)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"error: cannot write the report: {error}", file=sys.stderr)
        return 2

    scope = summary["scope"]["lines"]
    print(f"Coverage_Scope lines: {scope['percent']:.2f}% "
          f"({scope['covered']}/{scope['total']}), {len(summary['files'])} files")
    print(f"wrote {os.path.join(args.output_dir, 'summary.json')} and "
          f"{os.path.join(args.output_dir, 'html', 'index.html')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
