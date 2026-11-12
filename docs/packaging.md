# Packaging

KytyPS5-GTK is packaged as a Flatpak with the App_ID `io.github.MrG00gle.KytyPS5_GTK`. The manifest
is `packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml`. The desktop entry, the AppStream
metainfo, and the icons are in `packaging/linux/`.

The Flatpak contains the launcher `kyty_launcher` (the application command), the emulator
`kyty_emulator` with its assets, the desktop entry, the metainfo, the icons, and the license files of
the project and of every component under `/app/share/licenses/io.github.MrG00gle.KytyPS5_GTK`. It
does not contain `kyty_updater`.

## Install and run

Each release on the [releases page](https://github.com/MrG00gle/KytyPS5-GTK/releases) has a
`KytyPS5-GTK-<date>-<commit>-x86_64.flatpak` bundle. CI also attaches a bundle to every workflow run
as the `flatpak-bundle` artifact; outside official releases, those bundles describe themselves as
development snapshots.

```bash
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./KytyPS5-GTK-<date>-<commit>-x86_64.flatpak
flatpak run io.github.MrG00gle.KytyPS5_GTK
```

Flatpak installs the GNOME 49 runtime and the graphics driver extension from Flathub when they are
missing. To run the emulator directly inside the sandbox:

```bash
flatpak run --command=kyty_emulator io.github.MrG00gle.KytyPS5_GTK --help
flatpak run --command=kyty_emulator io.github.MrG00gle.KytyPS5_GTK --game /games/ExampleGame
```

## Build from the manifest

The build needs `flatpak-builder` and, from Flathub, the GNOME 49 SDK and runtime and the LLVM 20
SDK extension:

```bash
flatpak install --user flathub org.gnome.Platform//49 org.gnome.Sdk//49 \
  org.freedesktop.Sdk.Extension.llvm20//25.08
```

The manifest builds the checkout it sits in (a `dir` source), so initialize the submodules first
(`git submodule update --init --recursive`). Every other source is pinned: git sources by commit and
the FFmpeg archive by SHA-256. The build itself runs without network access.

Run `flatpak-builder` from a directory outside the checkout. Its state directory (`.flatpak-builder/`
in the working directory by default) and the build directory would otherwise land in the checkout,
and the manifest's `dir` source would copy them into the next build:

```bash
mkdir -p ~/.cache/kyty-flatpak && cd ~/.cache/kyty-flatpak
flatpak-builder --force-clean --state-dir=state build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
```

To install the result into your user installation and run it:

```bash
cd ~/.cache/kyty-flatpak
flatpak-builder --user --install --force-clean --state-dir=state build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak run io.github.MrG00gle.KytyPS5_GTK
```

To make a single-file bundle instead, as CI does:

```bash
cd ~/.cache/kyty-flatpak
flatpak-builder --force-clean --state-dir=state --repo=repo build \
  <checkout>/packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak build-bundle repo kytyps5-gtk.flatpak io.github.MrG00gle.KytyPS5_GTK \
  --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./kytyps5-gtk.flatpak
```

`flatpak-builder --show-manifest <manifest>` prints the parsed manifest. It has no `--state-dir`
option and always writes `.flatpak-builder/` into its working directory, so run it from
`~/.cache/kyty-flatpak` too, never from the checkout.

The CTests `flatpak_manifest_structure`, `flatpak_pins_match_registry`, and
`flatpak_metainfo_content` check the manifest, its pins against `cmake/KytyDependencies.cmake`, and
the metainfo.

## Sandbox permissions

The manifest grants exactly these `finish-args`:

| Permission | Justification |
|---|---|
| `--share=ipc` | The X11 fallback needs the shared IPC namespace for X11 shared memory (MIT-SHM). |
| `--socket=wayland` | The launcher and the emulator show their windows as Wayland clients on Wayland sessions. |
| `--socket=fallback-x11` | On sessions without Wayland, the windows use X11. Flatpak grants the X11 socket only when no Wayland socket is available. |
| `--socket=pulseaudio` | Game audio output, microphone capture, and the DualSense speaker over USB, which is a USB audio device, all go through the host sound server. |
| `--device=all` | The emulator needs the GPU device nodes for Vulkan (`/dev/dri`, and the NVIDIA nodes), `/dev/input` for gamepads and their hotplug, and `/dev/hidraw*` for the DualSense haptics and lightbar over Bluetooth. No narrower device permission includes `/dev/hidraw*`. |
| `--share=network` | The launcher downloads the compatibility list and the cheat catalog, and games use the network when **Enable network connection** is on. |

The manifest grants no `--filesystem` permission, no `--talk-name`, and no access to the
`org.freedesktop.Flatpak` D-Bus service. Folder access goes through the desktop portals: the folder
chooser (FileChooser portal) gives the launcher and the emulator lasting access to each chosen game
folder through the Document portal, which keeps it across restarts and reboots. **Open game folder**
uses the OpenURI portal.

## Access to more folders

For a folder that is not chosen through the folder chooser, for example one that you pass to
`flatpak run --command=kyty_emulator … --game`, grant access with an override:

```bash
flatpak override --user --filesystem=/mnt/games io.github.MrG00gle.KytyPS5_GTK
```

Append `:ro` (`--filesystem=/mnt/games:ro`) for read-only access; compressing a game to a ZArchive
then fails, because the archive is written next to the game. `flatpak override --user --show
io.github.MrG00gle.KytyPS5_GTK` lists the overrides, and `flatpak override --user --reset
io.github.MrG00gle.KytyPS5_GTK` removes them.

## Data locations

Inside the sandbox, the settings file is `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/config/Kyty/Kyty.ini`
and the data directory is `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/data/Kyty`, with the save data,
trophies, cheats, logs, shader logs, dumps, the pipeline cache, and the RenderDoc captures. A
`Kyty.ini` in the working directory is ignored. The [configuration reference](configuration.md) and
the [architecture guide](architecture.md#data-directory) have the details.

## Updates

The Flatpak receives its updates through Flatpak, not through the launcher's Updater: it has no
`kyty_updater`, and the launcher shows no update controls and sends no update requests. Apply the
updates of the runtime and the graphics driver extensions with:

```bash
flatpak update
```

A release bundle carries no update source of its own, so a new KytyPS5-GTK release arrives as a new
bundle. Install it over the old one; the settings and data in
`~/.var/app/io.github.MrG00gle.KytyPS5_GTK` stay:

```bash
flatpak install --user --reinstall ./KytyPS5-GTK-<date>-<commit>-x86_64.flatpak
```

## Validation and lint exceptions

The CI `flatpak` job builds the bundle with flatpak-builder, then runs `flatpak-builder-lint` in
manifest mode on the manifest and in repository mode on the exported repository, `desktop-file-validate`
on the installed desktop entry, `appstreamcli validate --no-net` on the installed metainfo, and
`tools/checks/check_licenses.py` on the installed license files. It then installs the bundle and runs
`kyty_emulator --help` in it. The lint commands, as CI runs them:

```bash
flatpak-builder-lint --exceptions --user-exceptions packaging/flatpak/lint-exceptions.json \
  manifest packaging/flatpak/io.github.MrG00gle.KytyPS5_GTK.yml
flatpak-builder-lint --exceptions --user-exceptions packaging/flatpak/lint-exceptions.json \
  repo <repository directory>
```

Locally, `flatpak-builder-lint` is part of the `org.flatpak.Builder` Flatpak on Flathub
(`flatpak run --command=flatpak-builder-lint org.flatpak.Builder …`).

`packaging/flatpak/lint-exceptions.json` lists the lint errors that the project accepts, each with
its justification:

| Lint error | Mode | Justification |
|---|---|---|
| `appid-url-not-reachable` | manifest and repository | The linter derives the URL `https://github.com/mrg00gle/kytyps5_gtk` from the App_ID and expects it to resolve. The repository's name contains a hyphen (`KytyPS5-GTK`), which an App_ID cannot contain, so that URL can never match. The metainfo's homepage is the real repository URL, `https://github.com/MrG00gle/KytyPS5-GTK`. |
| `appstream-screenshots-not-mirrored-in-ostree` | repository | The metainfo screenshot is served from this repository (`https://raw.githubusercontent.com/MrG00gle/KytyPS5-GTK/main/docs/screenshots/kyty-launcher-main.png`). The check requires a `screenshots/x86_64` ref in the exported repository, which only exists when flatpak-builder mirrors the screenshots into it (`--mirror-screenshots-url`, which Flathub's build service passes). A single-file bundle built here has no such mirror. |

On every ref other than `main`, CI lints with a per-run copy of that file that also excepts
`appstream-missing-screenshots`: the screenshot URL points at `main` and resolves only after `dev`
is merged. Runs on `main` lint with the committed file unchanged, so a missing screenshot fails them.

The committed metainfo has an empty `<releases/>` list, because a commit cannot contain its own
release tag. On official runs (pushes to `main` of `MrG00gle/KytyPS5-GTK`), the `flatpak` job writes
one `<release>` entry per published GitHub release plus the tag that the run publishes, newest first,
into the build's copy of the metainfo, and the release job checks that the bundle has the entry for
its tag. Every other run writes the published releases plus its own tag as a `type="development"`
entry, because the linter rejects an empty release list.

## Runtime and its support window

The Flatpak targets GNOME 49: the runtime `org.gnome.Platform//49`, the SDK `org.gnome.Sdk//49`, and
the SDK extension `org.freedesktop.Sdk.Extension.llvm20//25.08` for Clang and lld (25.08 is the
freedesktop SDK branch that the GNOME 49 SDK is based on). GNOME runtimes get about a year of
support on Flathub. Move to a newer GNOME runtime when Flathub announces the end of life of GNOME 49
at the latest; `flatpak remote-info flathub org.gnome.Platform//49` then shows it, and
`flatpak-builder-lint` already warns with `runtime-update-available-…` while a newer runtime exists.
No CI check watches the date, so the maintainer has to.

To move to GNOME `<v>`:

1. Check that the runtime and the SDK exist on Flathub:
   `flatpak remote-info flathub org.gnome.Platform//<v>` and
   `flatpak remote-info flathub org.gnome.Sdk//<v>`.
2. Find the freedesktop SDK branch of the new SDK: in
   `flatpak remote-info --show-metadata flathub org.gnome.Sdk//<v>`, the
   `[Extension org.freedesktop.Sdk.Extension]` group has `version = <branch>`. Choose the LLVM
   extension that exists on that branch, for example
   `flatpak remote-info flathub org.freedesktop.Sdk.Extension.llvm20//<branch>`.
3. In the manifest, change `runtime-version` (the `sdk` follows it), the `sdk-extensions` entry, and
   the extension's directories in `build-options` (`append-path` and `prepend-ld-library-path`,
   `/usr/lib/sdk/llvm<N>/…`), and the comment there.
4. Update the constants of `tools/checks/flatpak_checks.py` (`RUNTIME_VERSION`, `LLVM_EXTENSION`) and
   its tests in `tools/checks/tests/test_flatpak_checks.py`, in the same commit.
5. Change the `flatpak` job's container image in `.github/workflows/build.yml` to
   `ghcr.io/flathub-infra/flatpak-github-actions:gnome-<v>` (check that the tag exists), and the same
   image in `tools/checks/workflow_check.py` and `tools/checks/tests/test_workflow_check.py`.
6. Update the GNOME version in this guide and in the README's system requirements.
7. Install the new SDK, runtime, and extension (`flatpak install --user flathub
   org.gnome.Platform//<v> org.gnome.Sdk//<v> org.freedesktop.Sdk.Extension.llvm<N>//<branch>`), then
   rebuild from scratch with `flatpak-builder --force-clean` as above, outside the checkout.
8. Run the lint in manifest and repository mode, `desktop-file-validate`, `appstreamcli validate
   --no-net`, and `check_licenses.py` on the new build, run it with `flatpak run`, and run the CTests
   `flatpak_manifest_structure`, `flatpak_pins_match_registry`, `flatpak_metainfo_content`, and
   `workflow_structure`.
