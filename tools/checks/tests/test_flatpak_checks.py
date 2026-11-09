"""Unit tests for tools/checks/flatpak_checks.py and dependency_registry.cmake (task 26.6).

The real packaging files pass every check, and mutated copies of them fail with a line that names
the problem. PyYAML is required: without it this module fails to import with the checker's
message. cmake is KYTY_CMAKE (set by the CTest) or the cmake on PATH.
"""

import contextlib
import copy
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

CHECKS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = Path(CHECKS_DIR).parent.parent
SCRIPT = os.path.join(CHECKS_DIR, "flatpak_checks.py")
REGISTRY_SCRIPT = os.path.join(CHECKS_DIR, "dependency_registry.cmake")
CMAKE = os.environ.get("KYTY_CMAKE") or shutil.which("cmake")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CHECKS_DIR)

import flatpak_checks  # noqa: E402

try:
    import yaml  # noqa: E402
except ImportError:
    raise ImportError(flatpak_checks.YAML_MISSING) from None

APP_ID = flatpak_checks.APP_ID
# The files the checks read, relative to the checkout.
FILES = (flatpak_checks.MANIFEST, flatpak_checks.METAINFO, flatpak_checks.DESKTOP,
         flatpak_checks.ICON_SVG, flatpak_checks.ICON_PNG, flatpak_checks.RESOURCE_ICON,
         flatpak_checks.SCREENSHOT_FILE)
# cmake/KytyDependencies.cmake's table without RapidCheck, which BUILD_TESTING=ON adds.
TABLE_ORDER = ["opus", "openxr", "xbyak", "zydis", "zstd", "zarchive_source",
               "ffmpeg_core_prebuilt"]
RAPIDCHECK = {"type": "git", "url": "https://github.com/emil-e/rapidcheck.git",
              "commit": "2c3c4365aca21ef4e612768fdc95b1ce8b39a651", "disable-submodules": True,
              "dest": "_prefetched/rapidcheck"}


def dump_registry(build_testing, output, cwd=REPO):
    return subprocess.run([CMAKE, f"-DBUILD_TESTING={build_testing}", f"-DOUTPUT={output}", "-P",
                           REGISTRY_SCRIPT], cwd=cwd, capture_output=True, text=True,
                          stdin=subprocess.DEVNULL, check=False)


def png_header(width, height):
    """The first bytes of a PNG file: the signature and an IHDR chunk."""
    ihdr = struct.pack(">II5B", width, height, 8, 6, 0, 0, 0)
    chunk = struct.pack(">I", len(ihdr)) + b"IHDR" + ihdr
    return flatpak_checks.PNG_SIGNATURE + chunk + struct.pack(">I", zlib.crc32(chunk[4:]))


class Checkout(unittest.TestCase):
    """A copy of the packaging files in a temporary checkout, the registry dumps, and a runner."""

    registry_off = None
    registry_on = None

    @classmethod
    def setUpClass(cls):
        if not CMAKE:
            raise RuntimeError("no cmake: set KYTY_CMAKE or put cmake on PATH")
        cls.dumps = tempfile.TemporaryDirectory()
        for name, value in (("registry_off", "OFF"), ("registry_on", "ON")):
            output = Path(cls.dumps.name) / f"{name}.json"
            result = dump_registry(value, output)
            if result.returncode != 0:
                raise RuntimeError(f"dependency_registry.cmake failed:\n{result.stderr}")
            setattr(cls, name, json.loads(output.read_text(encoding="utf-8")))

    @classmethod
    def tearDownClass(cls):
        cls.dumps.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name) / "checkout"
        for relative in FILES:
            (self.repo / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / relative, self.repo / relative)
        self.registry = Path(self.temp.name) / "registry.json"
        self.write_registry(self.registry_off)

    def tearDown(self):
        self.temp.cleanup()

    def write_registry(self, data):
        self.registry.write_text(json.dumps(data), encoding="utf-8")

    def run_check(self, check, repo=None, cmake=None):
        arguments = ["--repo", str(repo or self.repo), "--check", check]
        arguments += ["--cmake", cmake] if cmake else ["--registry", str(self.registry)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = flatpak_checks.main(arguments)
        return status, stdout.getvalue().splitlines(), stderr.getvalue()

    def manifest(self):
        return yaml.safe_load((self.repo / flatpak_checks.MANIFEST).read_text(encoding="utf-8"))

    def write_manifest(self, data):
        (self.repo / flatpak_checks.MANIFEST).write_text(yaml.safe_dump(data, sort_keys=False),
                                                         encoding="utf-8")

    def mutate_manifest(self, change):
        data = self.manifest()
        change(data)
        self.write_manifest(data)

    def replace_in(self, relative, old, new):
        path = self.repo / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new), encoding="utf-8")

    def assert_fails(self, check, *fragments):
        """The check exits 1, and one problem line holds every fragment."""
        status, lines, stderr = self.run_check(check)
        self.assertEqual(status, 1, (lines, stderr))
        self.assertTrue(lines[-1].startswith(f"FAILED: {flatpak_checks.CHECK_NAMES[check]}: "),
                        lines)
        matching = [line for line in lines[:-1] if all(f in line for f in fragments)]
        self.assertTrue(matching, f"no line with {fragments} in {lines}")
        return lines

    def assert_passes(self, check):
        status, lines, stderr = self.run_check(check)
        self.assertEqual(status, 0, (lines, stderr))
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith(f"PASS: {flatpak_checks.CHECK_NAMES[check]}: "), lines)


def kyty_module(data):
    return next(module for module in data["modules"] if module["name"] == "kyty")


def glslang_module(data):
    return next(module for module in data["modules"] if module["name"] == "glslang")


def kyty_source(data, name):
    return next(source for source in kyty_module(data)["sources"]
                if source.get("dest") == f"_prefetched/{name}")


class RegistryDump(Checkout):
    def test_off_dump_lists_the_table_entries_without_rapidcheck(self):
        data = self.registry_off
        self.assertIs(data["build_testing"], False)
        entries = {entry["name"]: entry for entry in data["entries"]}
        self.assertEqual([entry["name"] for entry in data["entries"]], TABLE_ORDER)
        self.assertEqual(entries["zydis"]["submodules"], ["dependencies/zycore"])
        self.assertEqual(entries["zstd"]["source_subdir"], "build/cmake")
        self.assertEqual(entries["zarchive_source"]["patches"],
                         ["3rdparty/patches/zarchive-reader.patch"])
        self.assertEqual(entries["ffmpeg_core_prebuilt"]["kind"], "URL")
        self.assertEqual(entries["ffmpeg_core_prebuilt"]["filename"], "ffmpeg-linux-x64.zip")
        for entry in data["entries"]:
            with self.subTest(entry=entry["name"]):
                if entry["kind"] == "GIT":
                    self.assertTrue(flatpak_checks.is_hex(entry["commit"], 40))
                    self.assertEqual(entry["sha256"], "")
                else:
                    self.assertTrue(flatpak_checks.is_hex(entry["sha256"], 64))
                    self.assertEqual(entry["commit"], "")

    def test_on_dump_adds_rapidcheck_without_submodules(self):
        data = self.registry_on
        self.assertIs(data["build_testing"], True)
        self.assertEqual([entry["name"] for entry in data["entries"]], TABLE_ORDER + ["rapidcheck"])
        rapidcheck = data["entries"][-1]
        self.assertIs(rapidcheck["no_submodules"], True)
        self.assertEqual(rapidcheck["commit"], RAPIDCHECK["commit"])

    def test_other_working_directory_fails(self):
        result = dump_registry("OFF", Path(self.temp.name) / "out.json", cwd=self.temp.name)
        self.assertNotEqual(result.returncode, 0)
        # CMake wraps the message; compare it with single spaces.
        self.assertIn("as the working directory", " ".join(result.stderr.split()))
        self.assertFalse((Path(self.temp.name) / "out.json").exists())

    def test_missing_output_fails(self):
        result = subprocess.run([CMAKE, "-DBUILD_TESTING=OFF", "-P", REGISTRY_SCRIPT], cwd=REPO,
                                capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OUTPUT is not set", " ".join(result.stderr.split()))

    def test_dump_written_with_build_testing_on_is_an_input_error(self):
        self.write_registry(self.registry_on)
        status, _, stderr = self.run_check("pins")
        self.assertEqual(status, 2)
        self.assertIn("BUILD_TESTING=True", stderr)


class RealFiles(Checkout):
    def test_the_checkout_passes_every_check(self):
        for check in ("pins", "manifest", "metainfo"):
            with self.subTest(check=check):
                status, lines, stderr = self.run_check(check, repo=REPO, cmake=CMAKE)
                self.assertEqual(status, 0, (lines, stderr))
                self.assertTrue(lines[-1].startswith("PASS: "), lines)

    def test_the_copies_pass_every_check(self):
        for check in ("pins", "manifest", "metainfo"):
            with self.subTest(check=check):
                self.assert_passes(check)

    def test_a_yaml_round_trip_passes(self):
        self.write_manifest(self.manifest())
        self.assert_passes("pins")
        self.assert_passes("manifest")


class PinMutations(Checkout):
    def test_wrong_commit(self):
        registry_commit = kyty_source(self.manifest(), "opus")["commit"]
        self.mutate_manifest(lambda data: kyty_source(data, "opus").update(commit="1" * 40))
        self.assert_fails("pins", "opus", "commit", "1" * 40, registry_commit)

    def test_wrong_url(self):
        self.mutate_manifest(
            lambda data: kyty_source(data, "zstd").update(url="https://example.com/zstd.git"))
        self.assert_fails("pins", "zstd", "url", "https://example.com/zstd.git")

    def test_tag_instead_of_commit(self):
        def change(data):
            source = kyty_source(data, "xbyak")
            del source["commit"]
            source["tag"] = "v7.0"
        self.mutate_manifest(change)
        self.assert_fails("pins", "xbyak.git has no commit")

    def test_short_commit(self):
        self.mutate_manifest(lambda data: kyty_source(data, "openxr").update(commit="f2448a87"))
        self.assert_fails("pins", "OpenXR-SDK.git", "is not a full 40-character")

    def test_missing_sha256(self):
        self.mutate_manifest(lambda data: kyty_source(data, "ffmpeg_core_prebuilt").pop("sha256"))
        self.assert_fails("pins", "ffmpeg-linux-x64.zip has no sha256")

    def test_wrong_sha256(self):
        self.mutate_manifest(
            lambda data: kyty_source(data, "ffmpeg_core_prebuilt").update(sha256="a" * 64))
        self.assert_fails("pins", "ffmpeg_core_prebuilt", "sha256", "a" * 64)

    def test_wrong_dest_filename(self):
        self.mutate_manifest(lambda data: kyty_source(data, "ffmpeg_core_prebuilt").update(
            {"dest-filename": "ffmpeg.zip"}))
        self.assert_fails("pins", "ffmpeg_core_prebuilt", "dest-filename", "ffmpeg.zip")

    def test_extra_rapidcheck_source(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"].append(dict(RAPIDCHECK)))
        self.assert_fails("pins", "_prefetched/rapidcheck", "not a registry entry active with "
                          "BUILD_TESTING=OFF")

    def test_missing_registry_source(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"].remove(
            kyty_source(data, "zydis")))
        self.assert_fails("pins",
                          "zydis: the kyty module has no source with dest _prefetched/zydis")

    def test_duplicate_registry_source(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"].append(
            copy.deepcopy(kyty_source(data, "opus"))))
        self.assert_fails("pins", "opus: the kyty module has 2 sources")

    def test_source_outside_prefetched(self):
        self.mutate_manifest(lambda data: kyty_source(data, "xbyak").update(dest="3rdparty/xbyak"))
        self.assert_fails("pins", "xbyak.git: dest '3rdparty/xbyak' is not _prefetched/")

    def test_spirv_tools_commit_not_from_known_good(self):
        def change(data):
            glslang_module(data)["sources"][1]["commit"] = "2" * 40
        self.mutate_manifest(change)
        self.assert_fails("pins", "SPIRV-Tools.git", "commit", "known_good.json")

    def test_other_glslang_tag(self):
        def change(data):
            glslang_module(data)["sources"][0]["tag"] = "16.1.0"
        self.mutate_manifest(change)
        self.assert_fails("pins", "glslang.git", "tag '16.1.0'")

    def test_second_dir_source(self):
        self.mutate_manifest(lambda data: glslang_module(data)["sources"].append(
            {"type": "dir", "path": "../../3rdparty/glslang"}))
        self.assert_fails("pins", "the only dir source allowed is the checkout")

    def test_checkout_dir_moved(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"][0].update(path=".."))
        lines = self.assert_fails("pins", "the only dir source allowed is the checkout")
        self.assertIn("the kyty module has 0 dir sources for the checkout (../..); expected one",
                      lines)

    def test_other_source_type(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"].append(
            {"type": "shell", "commands": ["true"]}))
        self.assert_fails("pins", "source type 'shell' is not allowed")


class ManifestMutations(Checkout):
    def test_extra_finish_arg(self):
        self.mutate_manifest(lambda data: data["finish-args"].append("--socket=x11"))
        self.assert_fails("manifest", "--socket=x11 is not in the design's permission set")

    def test_filesystem_home(self):
        self.mutate_manifest(lambda data: data["finish-args"].append("--filesystem=home"))
        self.assert_fails("manifest", "--filesystem=home is forbidden")

    def test_flatpak_service(self):
        self.mutate_manifest(
            lambda data: data["finish-args"].append("--talk-name=org.freedesktop.Flatpak"))
        self.assert_fails("manifest", "--talk-name=org.freedesktop.Flatpak is forbidden")

    def test_flatpak_service_elsewhere(self):
        self.mutate_manifest(lambda data: data["build-options"].update(
            {"build-args": ["--talk-name=org.freedesktop.Flatpak"]}))
        self.assert_fails("manifest", "the manifest names org.freedesktop.Flatpak")

    def test_missing_and_doubled_finish_args(self):
        def change(data):
            data["finish-args"].remove("--share=network")
            data["finish-args"].append("--device=all")
        self.mutate_manifest(change)
        self.assert_fails("manifest", "--share=network is missing")
        self.assert_fails("manifest", "--device=all appears 2 times")

    def test_wrong_app_id(self):
        self.mutate_manifest(lambda data: data.update({"app-id": "io.github.example.Kyty"}))
        self.assert_fails("manifest", "app-id", "io.github.example.Kyty")

    def test_development_runtime(self):
        self.mutate_manifest(lambda data: data.update({"runtime-version": "master"}))
        self.assert_fails("manifest", "runtime-version is 'master'", "numbered stable release")

    def test_unquoted_runtime_version(self):
        self.mutate_manifest(lambda data: data.update({"runtime-version": 49}))
        self.assert_fails("manifest", "runtime-version is 49")

    def test_llvm_path_of_another_extension(self):
        self.mutate_manifest(
            lambda data: data["build-options"].update({"append-path": "/usr/lib/sdk/llvm19/bin"}))
        self.assert_fails("manifest", "append-path", "/usr/lib/sdk/llvm20/bin")

    def test_other_llvm_extension(self):
        self.mutate_manifest(lambda data: data.update(
            {"sdk-extensions": ["org.freedesktop.Sdk.Extension.llvm21"]}))
        lines = self.assert_fails("manifest", "sdk-extensions", "llvm21")
        self.assertTrue([line for line in lines if "append-path" in line and "llvm21" in line])

    def test_gcc_instead_of_clang(self):
        self.mutate_manifest(lambda data: data["build-options"]["env"].update(CXX="g++"))
        self.assert_fails("manifest", "env CXX is 'g++'")

    def test_without_lld(self):
        self.mutate_manifest(lambda data: data["build-options"].pop("config-opts"))
        self.assert_fails("manifest", "lack -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld")

    def test_kyty_config_opts(self):
        self.mutate_manifest(lambda data: kyty_module(data)["config-opts"].remove(
            "-DBUILD_TESTING=OFF"))
        self.assert_fails("manifest", "kyty config-opts")

    def test_glslang_kept_in_the_package(self):
        self.mutate_manifest(lambda data: glslang_module(data).pop("cleanup"))
        self.assert_fails("manifest", "glslang cleanup is None")

    def test_extra_module(self):
        self.mutate_manifest(lambda data: data["modules"].insert(
            0, {"name": "extra", "buildsystem": "simple", "sources": []}))
        self.assert_fails("manifest", "modules is ['extra', 'glslang', 'kyty']")

    def test_checkout_skips_git(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"][0]["skip"].append(".git"))
        self.assert_fails("manifest", "skips .git")

    def test_checkout_keeps_the_agents_directory(self):
        self.mutate_manifest(lambda data: kyty_module(data)["sources"][0]["skip"].remove(".agents"))
        self.assert_fails("manifest", "does not skip .agents")

    def test_no_submodules_entry_without_disable_submodules(self):
        registry = copy.deepcopy(self.registry_off)
        registry["entries"][0]["no_submodules"] = True
        self.write_registry(registry)
        self.assert_fails("manifest", "opus: the registry declares it NO_SUBMODULES",
                          "disable-submodules: true")
        self.mutate_manifest(lambda data: kyty_source(data, "opus").update(
            {"disable-submodules": True}))
        self.assert_passes("manifest")

    def test_disable_submodules_on_an_entry_with_submodules(self):
        self.mutate_manifest(lambda data: kyty_source(data, "zydis").update(
            {"disable-submodules": True}))
        self.assert_fails("manifest", "zydis: disable-submodules is True", "dependencies/zycore")


class MetainfoMutations(Checkout):
    METAINFO = flatpak_checks.METAINFO

    def set_releases(self, text):
        self.replace_in(self.METAINFO, "<releases/>", f"<releases>\n{text}\n  </releases>")

    def test_missing_release_date(self):
        self.set_releases('<release version="0.3.0"/>')
        self.assert_fails("metainfo", "release 0.3.0 has no date")

    def test_release_without_version(self):
        self.set_releases('<release date="2026-11-01"/>')
        self.assert_fails("metainfo", "release #1 has no version")

    def test_releases_newest_first_pass(self):
        self.set_releases('<release version="0.3.0" date="2026-11-01"/>\n'
                          '<release version="0.2.0" date="2026-10-01"/>')
        self.assert_passes("metainfo")

    def test_releases_oldest_first(self):
        self.set_releases('<release version="0.2.0" date="2026-10-01"/>\n'
                          '<release version="0.3.0" date="2026-11-01"/>')
        self.assert_fails("metainfo", "release 0.3.0 (2026-11-01) is newer", "newest first")

    def test_release_date_not_a_date(self):
        for date in ("2026-13-01", "2026-1-01", "20261101", "2026-11-01T10:00:00"):
            with self.subTest(date=date):
                self.set_releases(f'<release version="0.3.0" date="{date}"/>')
                self.assert_fails("metainfo", "release 0.3.0 has date", "not a YYYY-MM-DD date")
                shutil.copyfile(REPO / self.METAINFO, self.repo / self.METAINFO)

    def test_no_releases_element(self):
        self.replace_in(self.METAINFO, "<releases/>", "")
        self.assert_fails("metainfo", "no <releases> element")

    def test_wrong_screenshot_url(self):
        self.replace_in(self.METAINFO, "/main/docs/screenshots/", "/dev/docs/screenshots/")
        self.assert_fails("metainfo", "the screenshot images are",
                          flatpak_checks.SCREENSHOT_URL.replace("/main/", "/dev/"))

    def test_missing_screenshot_file(self):
        (self.repo / flatpak_checks.SCREENSHOT_FILE).unlink()
        self.assert_fails("metainfo", "docs/screenshots/kyty-launcher-main.png", "not a PNG file")

    def test_screenshot_without_caption(self):
        self.replace_in(self.METAINFO, "<caption>The game library in the grid view</caption>", "")
        self.assert_fails("metainfo", "a screenshot has no caption")

    def test_wrong_project_license(self):
        self.replace_in(self.METAINFO, "<project_license>GPL-2.0-only", "<project_license>GPL-2.0+")
        self.assert_fails("metainfo", "project_license is 'GPL-2.0+'")

    def test_wrong_metadata_license(self):
        self.replace_in(self.METAINFO, "<metadata_license>CC0-1.0", "<metadata_license>MIT")
        self.assert_fails("metainfo", "metadata_license is 'MIT'")

    def test_wrong_id(self):
        self.replace_in(self.METAINFO, f"<id>{APP_ID}</id>", "<id>io.github.example.Kyty</id>")
        self.assert_fails("metainfo", "the metainfo id is 'io.github.example.Kyty'")

    def test_no_description_paragraph(self):
        path = self.repo / self.METAINFO
        text = path.read_text(encoding="utf-8")
        start, end = text.index("<description>"), text.index("</description>")
        path.write_text(text[:start] + "<description>" + text[end:], encoding="utf-8")
        self.assert_fails("metainfo", "the description has no paragraph")

    def test_no_oars_rating(self):
        self.replace_in(self.METAINFO, '<content_rating type="oars-1.1"/>', "")
        self.assert_fails("metainfo", "content_rating type is None")

    def test_empty_developer_name(self):
        self.replace_in(self.METAINFO, "<name>MrG00gle</name>", "<name> </name>")
        self.assert_fails("metainfo", "the developer has no name")

    def test_wrong_homepage(self):
        self.replace_in(self.METAINFO, '<url type="homepage">https://github.com/MrG00gle/',
                        '<url type="homepage">https://example.com/')
        self.assert_fails("metainfo", "the homepage URLs")

    def test_wrong_launchable(self):
        self.replace_in(self.METAINFO, f"{APP_ID}.desktop</launchable>",
                        "kyty.desktop</launchable>")
        self.assert_fails("metainfo", "the launchable", "kyty.desktop")

    def test_metainfo_not_xml(self):
        self.replace_in(self.METAINFO, "</component>", "")
        status, _, stderr = self.run_check("metainfo")
        self.assertEqual(status, 2)
        self.assertIn("is not valid XML", stderr)

    def test_desktop_entry_exec_icon_and_categories(self):
        desktop = flatpak_checks.DESKTOP
        for old, new, fragment in (
                ("Exec=kyty_launcher", "Exec=kyty_emulator", "Exec is 'kyty_emulator'"),
                (f"Icon={APP_ID}", "Icon=kyty", "Icon is 'kyty'"),
                ("Categories=Game;Emulator;", "Categories=Game;", "Categories is ['Game']")):
            with self.subTest(old=old):
                self.replace_in(desktop, old, new)
                self.assert_fails("metainfo", "the desktop entry", fragment)
                shutil.copyfile(REPO / desktop, self.repo / desktop)

    def test_svg_icon_differs_from_the_resource_icon(self):
        with open(self.repo / flatpak_checks.ICON_SVG, "a", encoding="utf-8") as icon:
            icon.write("\n")
        self.assert_fails("metainfo", "differs from the launcher's GResource icon")

    def test_png_icon_not_square_or_too_small(self):
        for width, height in ((256, 128), (64, 64)):
            with self.subTest(size=(width, height)):
                (self.repo / flatpak_checks.ICON_PNG).write_bytes(png_header(width, height))
                self.assert_fails("metainfo", f"is {width}x{height}", "square of at least 128")

    def test_png_icon_not_a_png(self):
        shutil.copyfile(REPO / flatpak_checks.ICON_SVG, self.repo / flatpak_checks.ICON_PNG)
        self.assert_fails("metainfo", f"{flatpak_checks.ICON_PNG} is not a PNG file")


class CommandLine(Checkout):
    def run_script(self, *arguments, pythonpath=None):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        if pythonpath is not None:
            environment["PYTHONPATH"] = pythonpath
        return subprocess.run([sys.executable, "-B", SCRIPT, *arguments], capture_output=True,
                              text=True, env=environment, stdin=subprocess.DEVNULL, check=False)

    def test_without_pyyaml_the_manifest_checks_fail(self):
        blocker = Path(self.temp.name) / "no-yaml"
        blocker.mkdir()
        (blocker / "yaml.py").write_text('raise ImportError("PyYAML is hidden by the test")\n',
                                         encoding="utf-8")
        for check in ("pins", "manifest"):
            with self.subTest(check=check):
                result = self.run_script("--repo", str(self.repo), "--check", check,
                                         "--registry", str(self.registry), pythonpath=str(blocker))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stderr.strip(), flatpak_checks.YAML_MISSING)
                self.assertEqual(result.stdout, "")
        result = self.run_script("--repo", str(self.repo), "--check", "metainfo",
                                 pythonpath=str(blocker))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_script_exit_status_and_output(self):
        self.replace_in(flatpak_checks.METAINFO, "<releases/>",
                        '<releases><release version="1.0"/></releases>')
        result = self.run_script("--repo", str(self.repo), "--check", "metainfo")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["release 1.0 has no date",
                          "FAILED: flatpak_metainfo_content: 1 problem(s)"])

    def test_failed_registry_dump_is_an_input_error(self):
        status, _, stderr = self.run_check("pins", cmake=CMAKE)
        self.assertEqual(status, 2)
        self.assertIn("dependency_registry.cmake", stderr)

    def test_script_requires_the_check(self):
        result = self.run_script("--repo", str(self.repo))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--check", result.stderr)


if __name__ == "__main__":
    unittest.main()
