"""Unit tests for tools/ci/publish_release.py (Requirements 16.9, 16.12), with a fake gh."""

import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(CI_DIR, "publish_release.py")
FAKE_GH = os.path.join(TESTS_DIR, "fake_gh.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)

import publish_release  # noqa: E402

REPO = "MrG00gle/KytyPS5-GTK"
TAG = "KytyPS5-GTK-2026-11-12-abcdef0"
SHA = "abcdef0123456789abcdef0123456789abcdef01"
ASSET_NAMES = (f"{TAG}-Linux-x86_64.tar.gz", f"{TAG}-x86_64.flatpak",
               f"{TAG}-Linux-x86_64.tar.gz.sha256", f"{TAG}-x86_64.flatpak.sha256")


class FakeGhRun(unittest.TestCase):
    """Four asset files, a fake gh on PATH with an initial release list, and a runner."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = self.temp.name
        self.assets = []
        for number, name in enumerate(ASSET_NAMES):
            path = os.path.join(root, name)
            with open(path, "wb") as handle:
                handle.write(f"asset {number}\n".encode() * (number + 1))
            self.assets.append(path)
        bin_dir = os.path.join(root, "bin")
        os.mkdir(bin_dir)
        wrapper = os.path.join(bin_dir, "gh")
        with open(wrapper, "w", encoding="utf-8") as handle:
            handle.write(f'#!/bin/sh\nexec "{sys.executable}" -B "{FAKE_GH}" "$@"\n')
        os.chmod(wrapper, 0o755)
        self.state_path = os.path.join(root, "state.json")
        self.log_path = os.path.join(root, "calls.log")
        self.environment = {"PATH": bin_dir + os.pathsep + os.environ.get("PATH", ""),
                            "FAKE_GH_STATE": self.state_path, "FAKE_GH_LOG": self.log_path,
                            "PYTHONDONTWRITEBYTECODE": "1"}
        self.set_releases([])

    def tearDown(self):
        self.temp.cleanup()

    def set_releases(self, releases, next_id=100):
        with open(self.state_path, "w", encoding="utf-8") as handle:
            json.dump({"next_id": next_id, "releases": releases}, handle)

    def releases(self):
        with open(self.state_path, encoding="utf-8") as handle:
            return json.load(handle)["releases"]

    def calls(self):
        if not os.path.exists(self.log_path):
            return []
        with open(self.log_path, encoding="utf-8") as handle:
            return [json.loads(line) for line in handle]

    def call_kinds(self):
        kinds = []
        for call in self.calls():
            if call[:2] == ["api", "--paginate"]:
                kinds.append("list")
            elif call[:3] == ["api", "-X", "DELETE"]:
                kinds.append("delete " + call[3].rsplit("/", 1)[1])
            else:
                kinds.append(call[1])
        return kinds

    def asset_records(self, corrupt=None):
        records = []
        for path in self.assets:
            with open(path, "rb") as handle:
                data = handle.read()
            digest = hashlib.sha256(data).hexdigest()
            if os.path.basename(path) == corrupt:
                digest = "0" * 64
            records.append({"name": os.path.basename(path), "size": len(data),
                            "digest": f"sha256:{digest}", "state": "uploaded"})
        return records

    def release(self, release_id, tag=TAG, draft=False, prerelease=False, assets=None):
        return {"id": release_id, "tag_name": tag, "name": tag, "draft": draft,
                "prerelease": prerelease, "latest": False,
                "assets": self.asset_records() if assets is None else assets}

    def publish(self, fail=None, assets=None):
        environment = dict(self.environment)
        if fail:
            environment["FAKE_GH_FAIL"] = fail
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, environment), contextlib.redirect_stdout(stdout):
            status = publish_release.main(["--repo", REPO, "--tag", TAG, "--target", SHA,
                                           *(assets or self.assets)])
        return status, stdout.getvalue().splitlines()


class Publish(FakeGhRun):
    def test_fresh_publish_creates_uploads_checks_and_publishes(self):
        status, lines = self.publish()
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.call_kinds(), ["list", "create", "upload", "list", "edit"])
        create, upload, edit = self.calls()[1], self.calls()[2], self.calls()[4]
        self.assertEqual(create, ["release", "create", TAG, "--repo", REPO, "--draft", "--target",
                                  SHA, "--title", TAG, "--generate-notes"])
        self.assertEqual(upload, ["release", "upload", TAG, *self.assets, "--repo", REPO])
        self.assertEqual(edit, ["release", "edit", TAG, "--repo", REPO, "--draft=false",
                                "--latest"])
        [release] = self.releases()
        self.assertFalse(release["draft"])
        self.assertTrue(release["latest"])
        self.assertEqual(sorted(asset["name"] for asset in release["assets"]), sorted(ASSET_NAMES))
        self.assertIn(f"Published release {TAG} as the latest release with 4 assets.", lines)

    def test_rerun_of_a_complete_release_changes_nothing(self):
        other = self.release(5, tag="KytyPS5-GTK-2026-11-01-1111111")
        self.set_releases([other, self.release(6, tag="x"), self.release(7)])
        status, lines = self.publish()
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.call_kinds(), ["list"])
        self.assertIn(f"Release {TAG} is already published with all 4 assets; nothing to do.",
                      lines)
        self.assertEqual(len(self.releases()), 3)

    def test_leftover_draft_is_deleted_first(self):
        other = self.release(5, tag="KytyPS5-GTK-2026-11-01-1111111")
        draft = self.release(7, draft=True, assets=self.asset_records()[:1])
        self.set_releases([draft, other])
        status, lines = self.publish()
        self.assertEqual(status, 0, lines)
        self.assertEqual(self.call_kinds(),
                         ["list", "list", "delete 7", "create", "upload", "list", "edit"])
        tags = sorted((release["tag_name"], release["draft"]) for release in self.releases())
        self.assertEqual(tags, [("KytyPS5-GTK-2026-11-01-1111111", False), (TAG, False)])
        self.assertIn(f"Deleted 1 leftover draft release(s) for {TAG}.", lines)

    def test_upload_failure_deletes_the_draft(self):
        status, lines = self.publish(fail="upload")
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list", "create", "upload", "list", "delete 100"])
        self.assertEqual(self.releases(), [])
        self.assertIn(f"FAILED: release {TAG} was not published", lines)

    def test_publish_failure_deletes_the_draft(self):
        status, lines = self.publish(fail="edit")
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(),
                         ["list", "create", "upload", "list", "edit", "list", "delete 100"])
        self.assertEqual(self.releases(), [])

    def test_create_failure_leaves_nothing(self):
        status, _ = self.publish(fail="create")
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list", "create", "list"])
        self.assertEqual(self.releases(), [])

    def test_uploaded_asset_with_a_wrong_digest_is_not_published(self):
        status, lines = self.publish(fail="corrupt")
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list", "create", "upload", "list", "list",
                                             "delete 100"])
        self.assertEqual(self.releases(), [])
        self.assertTrue(any("has digest sha256:" in line for line in lines), lines)

    def test_digest_mismatch_on_a_published_release_changes_nothing(self):
        assets = self.asset_records(corrupt=ASSET_NAMES[1])
        self.set_releases([self.release(7, assets=assets)])
        status, lines = self.publish()
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list"])
        self.assertEqual(len(self.releases()), 1)
        self.assertIn(f"error: the published release {TAG}: asset {ASSET_NAMES[1]} has digest "
                      f"sha256:{'0' * 64}, the build's file {self.asset_records()[1]['digest']}",
                      lines)
        self.assertIn(f"FAILED: release {TAG} is published but differs from this build; nothing "
                      "was changed", lines)

    def test_published_release_missing_an_asset_changes_nothing(self):
        self.set_releases([self.release(7, assets=self.asset_records()[:3])])
        status, lines = self.publish()
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list"])
        self.assertIn(f"error: the published release {TAG}: asset {ASSET_NAMES[3]} is missing",
                      lines)

    def test_published_prerelease_changes_nothing(self):
        self.set_releases([self.release(7, prerelease=True)])
        status, lines = self.publish()
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list"])
        self.assertIn(f"error: the published release {TAG}: it is a prerelease", lines)

    def test_list_failure_changes_nothing(self):
        status, lines = self.publish(fail="list")
        self.assertEqual(status, 1)
        self.assertEqual(self.call_kinds(), ["list"])
        self.assertIn(f"FAILED: release {TAG} was not published", lines)

    def test_release_without_a_digest_is_checked_by_name_and_size(self):
        assets = self.asset_records()
        for record in assets:
            del record["digest"]
        self.set_releases([self.release(7, assets=assets)])
        status, lines = self.publish()
        self.assertEqual(status, 0, lines)


class Usage(FakeGhRun):
    def test_missing_asset_is_a_usage_error(self):
        status = self.publish(assets=[os.path.join(self.temp.name, "absent")])[0]
        self.assertEqual(status, 2)
        self.assertEqual(self.calls(), [])

    def test_empty_asset_is_a_usage_error(self):
        open(self.assets[0], "wb").close()
        status = self.publish()[0]
        self.assertEqual(status, 2)
        self.assertEqual(self.calls(), [])

    def test_two_assets_with_one_name_are_a_usage_error(self):
        other = os.path.join(self.temp.name, "bin", ASSET_NAMES[0])
        with open(other, "wb") as handle:
            handle.write(b"x")
        status = self.publish(assets=[self.assets[0], other])[0]
        self.assertEqual(status, 2)

    def test_script_requires_the_tag(self):
        environment = dict(os.environ, **self.environment)
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--repo", REPO, "--target", SHA,
                                 self.assets[0]], capture_output=True, text=True, env=environment,
                                stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--tag", result.stderr)

    def test_script_publishes_through_gh_on_path(self):
        environment = dict(os.environ, **self.environment)
        result = subprocess.run([sys.executable, "-B", SCRIPT, "--repo", REPO, "--tag", TAG,
                                 "--target", SHA, *self.assets], capture_output=True, text=True,
                                env=environment, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"+ gh release create {TAG}", result.stdout)


if __name__ == "__main__":
    unittest.main()
