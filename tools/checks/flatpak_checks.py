#!/usr/bin/env python3
"""Check the Flatpak packaging files against the dependency registry and the design.

Task 26.6 (Requirements 14.1, 14.2, 14.7, 14.8, 14.12, 14.14, 15.9, 15.10). One check per --check
value; each CTest of the same name runs one of them:

  pins      flatpak_pins_match_registry. For every registry entry that a configure with
            BUILD_TESTING=OFF declares (cmake/KytyDependencies.cmake, read through
            tools/checks/dependency_registry.cmake), the kyty module has exactly one source with
            dest _prefetched/<name>: a git source with the entry's url and commit, or a file source
            with its url, sha256, and dest-filename. No source is there for any other name
            (RapidCheck is declared only with BUILD_TESTING=ON). The glslang module has exactly
            glslang 16.2.0 and the SPIRV-Tools and SPIRV-Headers commits that glslang's
            known_good.json names for that tag. Every git source has a 40-character commit, every
            file or archive source a SHA-256, and the only dir source is the checkout (../..).
  manifest  flatpak_manifest_structure. The App_ID and command; runtime and SDK at the numbered
            release 49; the llvm20 extension and the PATH entries of that extension; CC, CXX, and
            -fuse-ld=lld; the top-level cleanup; the glslang and kyty modules (glslang removed by
            cleanup; kyty's exact config-opts and its checkout source with .git kept);
            disable-submodules exactly for registry entries declared NO_SUBMODULES; and finish-args
            exactly the design's six, with no --filesystem, --talk-name, --own-name,
            --system-talk-name, --system-own-name, or org.freedesktop.Flatpak access.
  metainfo  flatpak_metainfo_content. The metainfo's id, licenses, name, summary, description
            paragraphs, the screenshot URL (and the committed screenshot it serves), OARS rating,
            developer name, homepage, and launchable; each <release> has a version and a
            YYYY-MM-DD date, newest first. The desktop entry's Type, Exec, Icon, and Categories. The
            SVG icon equals the launcher's GResource icon, and the PNG icon is a square of at least
            128 pixels.

pins and manifest read the manifest with PyYAML (yaml.safe_load). Without PyYAML they exit 1 with
"flatpak_checks.py needs PyYAML (python3-pyyaml)": they never pass without checking. metainfo needs
only the standard library.

Usage:
    flatpak_checks.py --repo CHECKOUT --check pins|manifest|metainfo
                      [--cmake CMAKE | --registry JSON]

pins and manifest run CMAKE (default: cmake) on dependency_registry.cmake with BUILD_TESTING=OFF in
CHECKOUT, or read JSON, a file that script wrote with BUILD_TESTING=OFF.

Exit status: 0 when the check passes, 1 when it finds problems (one line each) or PyYAML is
missing, 2 on an input error (an unreadable or malformed file, or a failed registry dump).
"""

import argparse
import datetime
import json
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

APP_ID = "io.github.MrG00gle.KytyPS5_GTK"
MANIFEST = f"packaging/flatpak/{APP_ID}.yml"
METAINFO = f"packaging/linux/{APP_ID}.metainfo.xml"
DESKTOP = f"packaging/linux/{APP_ID}.desktop"
ICON_SVG = f"packaging/linux/icons/{APP_ID}.svg"
ICON_PNG = f"packaging/linux/icons/{APP_ID}.png"
RESOURCE_ICON = f"src/launcher/gtk/resources/icons/{APP_ID}.svg"
REGISTRY_SCRIPT = "tools/checks/dependency_registry.cmake"
YAML_MISSING = "flatpak_checks.py needs PyYAML (python3-pyyaml)"

CHECK_NAMES = {
    "pins": "flatpak_pins_match_registry",
    "manifest": "flatpak_manifest_structure",
    "metainfo": "flatpak_metainfo_content",
}

PREFETCHED = "_prefetched"
CHECKOUT_PATH = "../.."
# flatpak-builder's state directory and the local-only directories never enter the build. .git is
# kept, so the Flatpak gets the version string of native builds.
CHECKOUT_SKIP_REQUIRED = (".flatpak-builder", "_Build", "build", ".kiro", ".agents")
# glslang 16.2.0 and the commits that its known_good.json names for its External/ directory.
GLSLANG_SOURCES = (
    {"type": "git", "url": "https://github.com/KhronosGroup/glslang.git", "tag": "16.2.0",
     "commit": "f0bd0257c308b9a26562c1a30c4748a0219cc951"},
    {"type": "git", "url": "https://github.com/KhronosGroup/SPIRV-Tools.git",
     "commit": "fbe4f3ad913c44fe8700545f8ffe35d1382b7093", "dest": "External/spirv-tools"},
    {"type": "git", "url": "https://github.com/KhronosGroup/SPIRV-Headers.git",
     "commit": "04f10f650d514df88b76d25e83db360142c7b174",
     "dest": "External/spirv-tools/external/spirv-headers"},
)

RUNTIME = "org.gnome.Platform"
SDK = "org.gnome.Sdk"
RUNTIME_VERSION = "49"
LLVM_EXTENSION = "org.freedesktop.Sdk.Extension.llvm20"
LLVM_EXTENSION_PATTERN = re.compile(r"org\.freedesktop\.Sdk\.Extension\.(llvm[0-9]+)")
LLD_OPTION = "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
CLEANUP = ["/include", "/lib/cmake", "/lib/pkgconfig", "*.a", "/share/doc", "/share/man"]
MODULES = ["glslang", "kyty"]
KYTY_CONFIG_OPTS = ["-DCMAKE_BUILD_TYPE=Release", "-DKYTY_FLATPAK_BUILD=ON", "-DBUILD_TESTING=OFF",
                    "-DKYTY_PREFETCHED_SOURCES_DIR:PATH=_prefetched"]
# The design's permission set, each one justified in the packaging guide (Requirement 15.9).
FINISH_ARGS = ("--share=ipc", "--socket=wayland", "--socket=fallback-x11", "--socket=pulseaudio",
               "--device=all", "--share=network")
# Requirement 15.10 and the design: no file system access, no D-Bus names, no Flatpak service.
FORBIDDEN_ARG = re.compile(r"--(?:filesystem|talk-name|own-name|system-talk-name|system-own-name)"
                           r"(?:=|$)")
FLATPAK_SERVICE = "org.freedesktop.Flatpak"

METADATA_LICENSE = "CC0-1.0"
PROJECT_LICENSE = "GPL-2.0-only"
HOMEPAGE = "https://github.com/MrG00gle/KytyPS5-GTK"
SCREENSHOT_FILE = "docs/screenshots/kyty-launcher-main.png"
SCREENSHOT_URL = f"https://raw.githubusercontent.com/MrG00gle/KytyPS5-GTK/main/{SCREENSHOT_FILE}"
OARS = "oars-1.1"
DESKTOP_EXEC = "kyty_launcher"
DESKTOP_CATEGORIES = ["Game", "Emulator"]
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MIN_ICON_SIZE = 128
DATE_PATTERN = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")


class InputError(Exception):
    """A file cannot be read or parsed, or the registry dump failed."""


class MissingYaml(Exception):
    """PyYAML is not installed."""


def is_hex(value, length):
    return isinstance(value, str) and len(value) == length and re.fullmatch("[0-9a-f]+", value)


def read_text(path):
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from None


def read_bytes(path):
    try:
        return path.read_bytes()
    except OSError as error:
        raise InputError(f"cannot read {path}: {error}") from None


def load_manifest(path):
    # Imported here, so that the metainfo check runs without PyYAML.
    try:
        import yaml
    except ImportError:
        raise MissingYaml() from None
    text = read_text(path)
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as error:
        raise InputError(f"{path} is not valid YAML: {error}") from None
    if not isinstance(data, dict):
        raise InputError(f"{path}: the top level is not a mapping")
    return data


def load_registry(repo, cmake, registry):
    """The registry entries that a configure with BUILD_TESTING=OFF declares, in table order."""
    if registry is not None:
        text = read_text(registry)
        origin = str(registry)
    else:
        with tempfile.TemporaryDirectory(prefix="flatpak-checks-") as temp:
            output = Path(temp) / "registry.json"
            command = [cmake, "-DBUILD_TESTING=OFF", f"-DOUTPUT={output}", "-P",
                       str(repo / REGISTRY_SCRIPT)]
            try:
                result = subprocess.run(command, cwd=repo, capture_output=True, text=True,
                                        stdin=subprocess.DEVNULL, check=False)
            except OSError as error:
                raise InputError(f"cannot run {cmake}: {error}") from None
            if result.returncode != 0:
                raise InputError(f"{' '.join(command)} exited with {result.returncode}:\n"
                                 f"{(result.stdout + result.stderr).strip()}")
            text = read_text(output)
        origin = REGISTRY_SCRIPT
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise InputError(f"{origin}: not JSON: {error}") from None
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        raise InputError(f"{origin}: no entries list")
    if data.get("build_testing") is not False:
        raise InputError(f"{origin}: written with BUILD_TESTING={data.get('build_testing')!r}; the "
                         "kyty module configures with BUILD_TESTING=OFF")
    for entry in data["entries"]:
        if not isinstance(entry, dict) or not {"name", "kind", "url"} <= entry.keys():
            raise InputError(f"{origin}: malformed entry {entry!r}")
    return data["entries"]


def all_modules(modules, problems):
    """Every module mapping, nested ones included, in manifest order."""
    if not isinstance(modules, list):
        problems.append(f"modules is {modules!r}, not a list")
        return
    for module in modules:
        if not isinstance(module, dict):
            problems.append(f"module {module!r} is not a mapping (module files are not supported: "
                            "every module and source is checked in this manifest)")
            continue
        yield module
        yield from all_modules(module.get("modules", []), problems)


def module_sources(module, problems):
    """The source mappings of <module>; a problem for every other source."""
    name = module.get("name", "<unnamed>")
    sources = module.get("sources", [])
    if not isinstance(sources, list):
        problems.append(f"{name}: sources is {sources!r}, not a list")
        return []
    mappings = []
    for source in sources:
        if isinstance(source, dict):
            mappings.append(source)
        else:
            problems.append(f"{name}: source {source!r} is not a mapping (source files are not "
                            "supported: every source is checked in this manifest)")
    return mappings


def describe(module_name, source):
    location = source.get("url", source.get("path", "?"))
    return f"{module_name}: {source.get('type')} source {location}"


def find_module(modules, name):
    found = [module for module in modules if module.get("name") == name]
    return found[0] if len(found) == 1 else None


def prefetched_sources(kyty_sources):
    """dest name -> the kyty sources with dest _prefetched/<name>."""
    by_name = {}
    for source in kyty_sources:
        dest = source.get("dest")
        if isinstance(dest, str) and dest.startswith(PREFETCHED + "/"):
            by_name.setdefault(dest[len(PREFETCHED) + 1:], []).append(source)
    return by_name


def check_pins(manifest, registry):
    problems = []
    modules = list(all_modules(manifest.get("modules", []), problems))
    checkout_sources = 0
    for module in modules:
        name = module.get("name", "<unnamed>")
        for source in module_sources(module, problems):
            what = describe(name, source)
            kind = source.get("type")
            if kind == "git":
                commit = source.get("commit")
                if commit is None:
                    problems.append(f"{what} has no commit (a tag or branch is not a pin)")
                elif not is_hex(commit, 40):
                    problems.append(f"{what}: commit {commit!r} is not a full 40-character "
                                    "lowercase hexadecimal commit")
            elif kind in ("file", "archive"):
                sha256 = source.get("sha256")
                if sha256 is None:
                    problems.append(f"{what} has no sha256")
                elif not is_hex(sha256, 64):
                    problems.append(f"{what}: sha256 {sha256!r} is not 64 lowercase hexadecimal "
                                    "digits")
            elif kind == "dir":
                if name == "kyty" and source.get("path") == CHECKOUT_PATH:
                    checkout_sources += 1
                else:
                    problems.append(f"{what}: the only dir source allowed is the checkout, path "
                                    f"{CHECKOUT_PATH} in the kyty module")
            else:
                problems.append(f"{what}: source type {kind!r} is not allowed (git, file, archive, "
                                "or the checkout dir)")
    if checkout_sources != 1:
        problems.append(f"the kyty module has {checkout_sources} dir sources for the checkout "
                        f"({CHECKOUT_PATH}); expected one")

    kyty = find_module(modules, "kyty")
    if kyty is None:
        problems.append("the manifest has no single kyty module")
    else:
        kyty_sources = module_sources(kyty, [])
        for source in kyty_sources:
            if source.get("type") != "dir" and not prefetched_sources([source]):
                problems.append(f"{describe('kyty', source)}: dest {source.get('dest')!r} is not "
                                f"{PREFETCHED}/<registry entry>")
        by_name = prefetched_sources(kyty_sources)
        active = {entry["name"] for entry in registry}
        for entry in registry:
            problems.extend(registry_entry_problems(entry, by_name.get(entry["name"], [])))
        for name, sources in by_name.items():
            if name not in active:
                for source in sources:
                    problems.append(
                        f"kyty: source {PREFETCHED}/{name} ({source.get('url')}) is not a registry "
                        "entry active with BUILD_TESTING=OFF, the value the kyty module configures "
                        "with")

    glslang = find_module(modules, "glslang")
    if glslang is None:
        problems.append("the manifest has no single glslang module")
    else:
        problems.extend(glslang_problems(module_sources(glslang, [])))

    git = sum(1 for entry in registry if entry["kind"] == "GIT")
    summary = (f"{len(registry)} registry entries ({git} git, {len(registry) - git} file) and "
               f"{len(GLSLANG_SOURCES)} glslang sources pinned as expected; the only dir source is "
               f"{CHECKOUT_PATH}")
    return problems, summary


def registry_entry_problems(entry, sources):
    name = entry["name"]
    dest = f"{PREFETCHED}/{name}"
    if not sources:
        return [f"{name}: the kyty module has no source with dest {dest} ({entry['url']})"]
    problems = []
    if len(sources) > 1:
        problems.append(f"{name}: the kyty module has {len(sources)} sources with dest {dest}; "
                        "expected one")
    source = sources[0]
    if entry["kind"] == "GIT":
        wanted = {"type": "git", "url": entry["url"], "commit": entry.get("commit")}
    else:
        wanted = {"type": "file", "url": entry["url"], "sha256": entry.get("sha256"),
                  "dest-filename": entry.get("filename")}
    for key, value in wanted.items():
        if source.get(key) != value:
            problems.append(f"{name}: manifest {key} {source.get(key)!r} differs from the "
                            f"registry's {value!r}")
    return problems


def glslang_problems(sources):
    problems = []
    urls = [source.get("url") for source in sources]
    for wanted in GLSLANG_SOURCES:
        count = urls.count(wanted["url"])
        if count != 1:
            problems.append(f"glslang: {count} sources {wanted['url']}; expected one")
            continue
        source = sources[urls.index(wanted["url"])]
        for key, value in wanted.items():
            if source.get(key) != value:
                problems.append(f"glslang: {wanted['url']}: {key} {source.get(key)!r}, expected "
                                f"{value!r} (glslang 16.2.0 and its known_good.json)")
    wanted_urls = [wanted["url"] for wanted in GLSLANG_SOURCES]
    for source in sources:
        if source.get("url") not in wanted_urls:
            problems.append(f"{describe('glslang', source)} is not glslang 16.2.0 or one of its "
                            "known_good.json sources")
    return problems


def strings_in(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from strings_in(key)
            yield from strings_in(item)
    elif isinstance(value, list):
        for item in value:
            yield from strings_in(item)


def check_manifest(manifest, registry):
    problems = []

    def expect(what, actual, wanted, why=""):
        if actual != wanted:
            problems.append(f"{what} is {actual!r}, expected {wanted!r}{why}")

    expect("app-id", manifest.get("app-id"), APP_ID)
    expect("command", manifest.get("command"), DESKTOP_EXEC)
    expect("runtime", manifest.get("runtime"), RUNTIME)
    expect("sdk", manifest.get("sdk"), SDK)
    expect("runtime-version", manifest.get("runtime-version"), RUNTIME_VERSION,
           " (the numbered stable release as a string, not master or another branch)")
    extensions = manifest.get("sdk-extensions")
    expect("sdk-extensions", extensions, [LLVM_EXTENSION])
    options = manifest.get("build-options")
    if not isinstance(options, dict):
        problems.append(f"build-options is {options!r}, not a mapping")
        options = {}
    llvm = [LLVM_EXTENSION_PATTERN.fullmatch(extension) for extension in extensions or []
            if isinstance(extension, str)]
    llvm = [match.group(1) for match in llvm if match]
    if len(llvm) == 1:
        why = f" (the {llvm[0]} SDK extension's directory)"
        expect("build-options append-path", options.get("append-path"),
               f"/usr/lib/sdk/{llvm[0]}/bin", why)
        expect("build-options prepend-ld-library-path", options.get("prepend-ld-library-path"),
               f"/usr/lib/sdk/{llvm[0]}/lib", why)
    else:
        problems.append(f"sdk-extensions {extensions!r} name {len(llvm)} llvm extensions; "
                        "expected one")
    env = options.get("env") if isinstance(options.get("env"), dict) else {}
    expect("build-options env CC", env.get("CC"), "clang")
    expect("build-options env CXX", env.get("CXX"), "clang++")
    config_opts = options.get("config-opts")
    if not isinstance(config_opts, list) or LLD_OPTION not in config_opts:
        problems.append(f"build-options config-opts {config_opts!r} lack {LLD_OPTION}")
    expect("cleanup", manifest.get("cleanup"), CLEANUP)
    problems.extend(finish_args_problems(manifest.get("finish-args")))
    # finish-args are checked above; the service may appear nowhere else either.
    rest = {key: value for key, value in manifest.items() if key != "finish-args"}
    for text in strings_in(rest):
        if FLATPAK_SERVICE in text:
            problems.append(f"the manifest names {FLATPAK_SERVICE} ({text!r}); no access to it is "
                            "allowed")

    modules = manifest.get("modules")
    if not isinstance(modules, list):
        problems.append(f"modules is {modules!r}, not a list")
        modules = []
    expect("modules", [module.get("name") if isinstance(module, dict) else module
                       for module in modules], MODULES)
    modules = [module for module in modules if isinstance(module, dict)]
    glslang = find_module(modules, "glslang")
    if glslang is not None:
        expect("glslang buildsystem", glslang.get("buildsystem"), "cmake-ninja")
        expect("glslang cleanup", glslang.get("cleanup"), ["*"],
               " (glslang is needed only to build; nothing of it is kept in the package)")
    kyty = find_module(modules, "kyty")
    if kyty is not None:
        expect("kyty buildsystem", kyty.get("buildsystem"), "cmake-ninja")
        expect("kyty builddir", kyty.get("builddir"), True)
        expect("kyty config-opts", kyty.get("config-opts"), KYTY_CONFIG_OPTS)
        sources = module_sources(kyty, problems)
        problems.extend(checkout_problems(sources))
        problems.extend(submodule_problems(registry, prefetched_sources(sources)))
    summary = (f"{APP_ID}, {RUNTIME}//{RUNTIME_VERSION} with {LLVM_EXTENSION}, modules "
               f"{', '.join(MODULES)}, finish-args exactly {' '.join(FINISH_ARGS)}")
    return problems, summary


def finish_args_problems(args):
    if not isinstance(args, list):
        return [f"finish-args is {args!r}, not a list"]
    problems = []
    for arg in args:
        if not isinstance(arg, str):
            problems.append(f"finish-args: {arg!r} is not a string")
        elif FORBIDDEN_ARG.match(arg) or FLATPAK_SERVICE in arg:
            problems.append(f"finish-args: {arg} is forbidden (no --filesystem, --talk-name, "
                            f"--own-name, --system-talk-name, --system-own-name, or "
                            f"{FLATPAK_SERVICE} access)")
        elif arg not in FINISH_ARGS:
            problems.append(f"finish-args: {arg} is not in the design's permission set")
    for arg in FINISH_ARGS:
        count = args.count(arg)
        if count == 0:
            problems.append(f"finish-args: {arg} is missing")
        elif count > 1:
            problems.append(f"finish-args: {arg} appears {count} times")
    return problems


def checkout_problems(sources):
    if not sources or sources[0].get("type") != "dir" or sources[0].get("path") != CHECKOUT_PATH:
        return [f"the first kyty source is not the checkout (type dir, path {CHECKOUT_PATH})"]
    skip = sources[0].get("skip", [])
    if not isinstance(skip, list):
        return [f"the checkout source's skip is {skip!r}, not a list"]
    problems = [f"the checkout source does not skip {name}" for name in CHECKOUT_SKIP_REQUIRED
                if name not in skip]
    if any(isinstance(name, str) and name.strip("/").split("/")[0] == ".git" for name in skip):
        problems.append("the checkout source skips .git, which the version string needs")
    return problems


def submodule_problems(registry, by_name):
    problems = []
    for entry in registry:
        if entry["kind"] != "GIT" or len(by_name.get(entry["name"], [])) != 1:
            continue
        disabled = by_name[entry["name"]][0].get("disable-submodules", False)
        if entry.get("no_submodules"):
            if disabled is not True:
                problems.append(f"{entry['name']}: the registry declares it NO_SUBMODULES, so its "
                                "git source needs disable-submodules: true")
        elif disabled is not False:
            submodules = ", ".join(entry.get("submodules") or []) or "every submodule"
            problems.append(f"{entry['name']}: disable-submodules is {disabled!r}, but the "
                            f"registry clones {submodules}")
    return problems


def element_text(element):
    return "".join(element.itertext()).strip() if element is not None else None


def png_size(data):
    if data[:8] != PNG_SIGNATURE or len(data) < 24 or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def read_desktop_entry(path):
    entries = {}
    group = None
    for line in read_text(path).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
        elif group == "Desktop Entry" and "=" in line:
            key, _, value = line.partition("=")
            entries[key.strip()] = value.strip()
    return entries


def check_metainfo(repo):
    problems = []

    def expect(what, actual, wanted):
        if actual != wanted:
            problems.append(f"{what} is {actual!r}, expected {wanted!r}")

    path = repo / METAINFO
    try:
        root = ET.fromstring(read_text(path))
    except ET.ParseError as error:
        raise InputError(f"{path} is not valid XML: {error}") from None
    expect("the metainfo root element", root.tag, "component")
    expect("the component type", root.get("type"), "desktop-application")
    expect("the metainfo id", element_text(root.find("id")), APP_ID)
    expect("metadata_license", element_text(root.find("metadata_license")), METADATA_LICENSE)
    expect("project_license", element_text(root.find("project_license")), PROJECT_LICENSE)
    for tag in ("name", "summary"):
        if not element_text(root.find(tag)):
            problems.append(f"the metainfo has no {tag}")
    paragraphs = root.findall("description/p")
    if not paragraphs:
        problems.append("the description has no paragraph")
    for number, paragraph in enumerate(paragraphs, 1):
        if not element_text(paragraph):
            problems.append(f"description paragraph {number} is empty")
    problems.extend(screenshot_problems(repo, root))
    rating = root.find("content_rating")
    expect("the content_rating type", rating.get("type") if rating is not None else None, OARS)
    if not element_text(root.find("developer/name")):
        problems.append("the developer has no name")
    homepages = [element_text(url) for url in root.findall("url") if url.get("type") == "homepage"]
    expect("the homepage URLs", homepages, [HOMEPAGE])
    launchable = root.find("launchable")
    expect("the launchable", (launchable.get("type"), element_text(launchable))
           if launchable is not None else None, ("desktop-id", f"{APP_ID}.desktop"))
    releases, release_problems = releases_problems(root)
    problems.extend(release_problems)

    desktop = read_desktop_entry(repo / DESKTOP)
    expect("the desktop entry Type", desktop.get("Type"), "Application")
    expect("the desktop entry Exec", desktop.get("Exec"), DESKTOP_EXEC)
    expect("the desktop entry Icon", desktop.get("Icon"), APP_ID)
    categories = [name for name in desktop.get("Categories", "").split(";") if name]
    expect("the desktop entry Categories", categories, DESKTOP_CATEGORIES)
    if not desktop.get("Name"):
        problems.append("the desktop entry has no Name")

    if read_bytes(repo / ICON_SVG) != read_bytes(repo / RESOURCE_ICON):
        problems.append(f"{ICON_SVG} differs from the launcher's GResource icon {RESOURCE_ICON}")
    size = png_size(read_bytes(repo / ICON_PNG))
    if size is None:
        problems.append(f"{ICON_PNG} is not a PNG file")
    elif size[0] != size[1] or size[0] < MIN_ICON_SIZE:
        problems.append(f"{ICON_PNG} is {size[0]}x{size[1]}; expected a square of at least "
                        f"{MIN_ICON_SIZE} pixels")
    png_text = f"{size[0]}x{size[1]}" if size else "?"
    summary = (f"{APP_ID} metainfo ({len(paragraphs)} description paragraphs, screenshot "
               f"{SCREENSHOT_URL}, {releases} release entries), desktop entry, SVG icon equal to "
               f"the GResource icon, {png_text} PNG icon")
    return problems, summary


def screenshot_problems(repo, root):
    problems = []
    screenshots = root.findall("screenshots/screenshot")
    if len(screenshots) != 1:
        problems.append(f"the metainfo has {len(screenshots)} screenshots; expected one")
    images = [element_text(image) for image in root.findall("screenshots/screenshot/image")]
    if images != [SCREENSHOT_URL]:
        problems.append(f"the screenshot images are {images!r}, expected [{SCREENSHOT_URL!r}]")
    for screenshot in screenshots:
        if screenshot.get("type") != "default":
            problems.append(f"screenshot type is {screenshot.get('type')!r}, expected 'default'")
        if not element_text(screenshot.find("caption")):
            problems.append("a screenshot has no caption")
    path = repo / SCREENSHOT_FILE
    if png_size(read_bytes(path) if path.is_file() else b"") is None:
        problems.append(f"{SCREENSHOT_FILE}, which the screenshot URL serves, is not a PNG file in "
                        "the checkout")
    return problems


def releases_problems(root):
    releases = root.find("releases")
    if releases is None:
        return 0, ["the metainfo has no <releases> element (one <release> per GitHub release)"]
    problems = []
    previous = None
    entries = releases.findall("release")
    for number, release in enumerate(entries, 1):
        version = release.get("version")
        label = version or f"#{number}"
        if not version:
            problems.append(f"release #{number} has no version")
        date_text = release.get("date")
        if not date_text:
            problems.append(f"release {label} has no date")
            continue
        try:
            if not DATE_PATTERN.fullmatch(date_text):
                raise ValueError
            date = datetime.date.fromisoformat(date_text)
        except ValueError:
            problems.append(f"release {label} has date {date_text!r}, not a YYYY-MM-DD date")
            continue
        if previous is not None and date > previous:
            problems.append(f"release {label} ({date_text}) is newer than the release before it; "
                            "list releases newest first")
        previous = date
    return len(entries), problems


def run_check(check, repo, cmake, registry):
    if check == "metainfo":
        return check_metainfo(repo)
    manifest = load_manifest(repo / MANIFEST)
    entries = load_registry(repo, cmake, registry)
    if check == "pins":
        return check_pins(manifest, entries)
    return check_manifest(manifest, entries)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the Flatpak manifest, metainfo, desktop entry, and icons; print each "
        "problem and exit 1 if there is one.")
    parser.add_argument("--repo", type=Path, required=True, help="the checkout")
    parser.add_argument("--check", choices=sorted(CHECK_NAMES), required=True)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--cmake", default="cmake",
                        help="the cmake that runs dependency_registry.cmake (default: cmake)")
    source.add_argument("--registry", type=Path,
                        help="a dependency_registry.cmake output written with BUILD_TESTING=OFF")
    args = parser.parse_args(argv)

    name = CHECK_NAMES[args.check]
    try:
        problems, summary = run_check(args.check, args.repo, args.cmake, args.registry)
    except MissingYaml:
        print(YAML_MISSING, file=sys.stderr)
        return 1
    except InputError as error:
        print(f"error: {name}: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(problem)
    if problems:
        print(f"FAILED: {name}: {len(problems)} problem(s)")
        return 1
    print(f"PASS: {name}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
