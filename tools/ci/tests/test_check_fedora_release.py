"""Unit tests for tools/ci/check_fedora_release.py (Requirement 16.2)."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

CI_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(CI_DIR, "check_fedora_release.py")
# Keep the local import below from writing __pycache__ into the checkout.
sys.dont_write_bytecode = True
sys.path.insert(0, CI_DIR)

import check_fedora_release  # noqa: E402

# /etc/os-release of the fedora:44 container image (a GA release).
GA = """NAME="Fedora Linux"
VERSION="44 (Container Image)"
RELEASE_TYPE=stable
ID=fedora
VERSION_ID=44
VERSION_CODENAME=""
PRETTY_NAME="Fedora Linux 44 (Container Image)"
ANSI_COLOR="0;38;2;60;110;180"
LOGO=fedora-logo-icon
CPE_NAME="cpe:/o:fedoraproject:fedora:44"
DEFAULT_HOSTNAME="fedora"
HOME_URL="https://fedoraproject.org/"
DOCUMENTATION_URL="https://docs.fedoraproject.org/en-US/fedora/f44/"
SUPPORT_URL="https://ask.fedoraproject.org/"
BUG_REPORT_URL="https://bugzilla.redhat.com/"
REDHAT_BUGZILLA_PRODUCT="Fedora"
REDHAT_BUGZILLA_PRODUCT_VERSION=44
REDHAT_SUPPORT_PRODUCT="Fedora"
REDHAT_SUPPORT_PRODUCT_VERSION=44
SUPPORT_END=2027-05-19
VARIANT="Container Image"
VARIANT_ID=container
"""

# fedora:rawhide: a release number, but development and named Rawhide.
RAWHIDE = """NAME="Fedora Linux"
VERSION="46 (Container Image Prerelease)"
RELEASE_TYPE=development
ID=fedora
VERSION_ID=46
PRETTY_NAME="Fedora Linux 46 (Container Image Prerelease)"
REDHAT_BUGZILLA_PRODUCT="Fedora"
REDHAT_BUGZILLA_PRODUCT_VERSION=rawhide
REDHAT_SUPPORT_PRODUCT="Fedora"
REDHAT_SUPPORT_PRODUCT_VERSION=rawhide
VARIANT="Container Image"
VARIANT_ID=container
"""

# A Beta image.
BETA = GA.replace('VERSION="44 (Container Image)"', 'VERSION="45 (Container Image Beta)"') \
    .replace("VERSION_ID=44", "VERSION_ID=45") \
    .replace('PRETTY_NAME="Fedora Linux 44 (Container Image)"',
             'PRETTY_NAME="Fedora Linux 45 (Container Image Beta)"')

# A branched release before its Beta: "Prerelease" with RELEASE_TYPE missing.
PRERELEASE = GA.replace('VERSION="44 (Container Image)"',
                        'VERSION="45 (Container Image Prerelease)"') \
    .replace("RELEASE_TYPE=stable\n", "") \
    .replace('PRETTY_NAME="Fedora Linux 44 (Container Image)"',
             'PRETTY_NAME="Fedora Linux 45 (Container Image Prerelease)"')


class Run(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.temp.name, "os-release")

    def tearDown(self):
        self.temp.cleanup()

    def check(self, text):
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(text)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = check_fedora_release.main([self.path])
        return status, stdout.getvalue().splitlines(), stderr.getvalue()


class Releases(Run):
    def test_ga_release_passes(self):
        status, lines, _ = self.check(GA)
        self.assertEqual(status, 0)
        self.assertEqual(lines, ["Fedora 44 is a GA release (Fedora Linux 44 (Container Image))"])

    def test_workstation_ga_release_passes(self):
        text = GA.replace("Container Image", "Workstation Edition")
        status, lines, _ = self.check(text)
        self.assertEqual(status, 0, lines)

    def test_ga_release_without_release_type_passes(self):
        status, lines, _ = self.check(GA.replace("RELEASE_TYPE=stable\n", ""))
        self.assertEqual(status, 0, lines)

    def test_rawhide_fails_with_every_reason(self):
        status, lines, _ = self.check(RAWHIDE)
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: RELEASE_TYPE is 'development', not 'stable'", lines)
        self.assertIn("not a GA release: REDHAT_SUPPORT_PRODUCT_VERSION is 'rawhide', which names "
                      "rawhide", lines)
        self.assertIn("not a GA release: VERSION is '46 (Container Image Prerelease)', which names "
                      "Prerelease", lines)

    def test_rawhide_version_id_fails(self):
        status, lines, _ = self.check(GA.replace("VERSION_ID=44", "VERSION_ID=rawhide"))
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: VERSION_ID is 'rawhide', not a release number", lines)

    def test_beta_fails(self):
        status, lines, _ = self.check(BETA)
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: VERSION is '45 (Container Image Beta)', which names Beta",
                      lines)

    def test_prerelease_fails(self):
        status, lines, _ = self.check(PRERELEASE)
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: PRETTY_NAME is 'Fedora Linux 45 (Container Image "
                      "Prerelease)', which names Prerelease", lines)

    def test_non_numeric_version_ids_fail(self):
        for version_id in ("44.1", "", "４４", "44a"):
            with self.subTest(version_id=version_id):
                text = GA.replace("VERSION_ID=44", f"VERSION_ID={version_id}")
                status, lines, _ = self.check(text)
                self.assertEqual(status, 1)
                self.assertTrue(any(line.startswith("not a GA release: VERSION_ID is")
                                    for line in lines), lines)

    def test_missing_version_id_fails(self):
        status, lines, _ = self.check(GA.replace("VERSION_ID=44\n", ""))
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: VERSION_ID is '', not a release number", lines)

    def test_other_distribution_fails(self):
        status, lines, _ = self.check(GA.replace("ID=fedora", "ID=centos"))
        self.assertEqual(status, 1)
        self.assertIn("not a GA release: ID is 'centos', not 'fedora'", lines)

    def test_words_inside_other_words_do_not_count(self):
        text = GA.replace('VARIANT="Container Image"', 'VARIANT="Alphabetagamma Image"')
        status, lines, _ = self.check(text)
        self.assertEqual(status, 0, lines)


class Parsing(Run):
    def test_quotes_comments_and_blank_lines(self):
        fields = check_fedora_release.parse_os_release(
            "# comment\n\nID='fedora'\nVERSION_ID=\"44\"\nNAME=\"Fedora \\\"Linux\\\"\"\n")
        self.assertEqual(fields["ID"], "fedora")
        self.assertEqual(fields["VERSION_ID"], "44")
        self.assertEqual(fields["NAME"], 'Fedora "Linux"')

    def test_line_without_equals_is_an_input_error(self):
        status, lines, stderr = self.check(GA + "garbage line\n")
        self.assertEqual(status, 2)
        self.assertEqual(lines, [])
        self.assertIn("is not KEY=value", stderr)

    def test_unbalanced_quote_is_an_input_error(self):
        status, _, stderr = self.check(GA + 'BROKEN="x\n')
        self.assertEqual(status, 2)
        self.assertIn("line", stderr)

    def test_missing_file_is_an_input_error(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = check_fedora_release.main([os.path.join(self.temp.name, "absent")])
        self.assertEqual(status, 2)
        self.assertIn("cannot read", stderr.getvalue())

    def test_script_exit_status(self):
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(RAWHIDE)
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, "-B", SCRIPT, self.path], capture_output=True,
                                text=True, env=environment, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(result.stdout.startswith("not a GA release: "))


if __name__ == "__main__":
    unittest.main()
