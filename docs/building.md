# Building KytyPS5-GTK

KytyPS5-GTK builds on Linux x86-64 with Clang, lld, CMake 3.22 or newer, and Ninja. The build
produces three programs:

- `kyty_emulator`, the emulator;
- `kyty_launcher`, the GTK 4 and libadwaita launcher;
- `kyty_updater`, the helper that installs updates of the release archive.

The `kyty_tests` target builds the test suite. The [testing guide](testing.md) shows how to run it.

## Toolchain

Only Clang with the lld linker is supported. Configure with `-DCMAKE_C_COMPILER=clang` and
`-DCMAKE_CXX_COMPILER=clang++`. The project links every program with lld and stops at configure
when lld is missing. GCC builds are not tested, and the coverage build stops at configure unless the
compiler is Clang. The project uses ccache automatically when it is installed.

The GTK launcher needs at least GTK 4.14, libadwaita 1.5, libsoup 3.4, and GLib 2.80, the versions
of Ubuntu 24.04. Older distributions can still build the emulator alone (see
[Emulator only](#emulator-only)).

## Dependencies

Each command installs everything needed to build the emulator, the launcher, the Updater, and the
test suite. A few packages are needed only by the tests: the headless display servers (weston,
Xvfb, and the D-Bus daemon) and Mesa's software Vulkan driver for the UI and GPU tests, GCC for the
test that checks the coverage build's Clang requirement, PyYAML for the packaging checks, and the
fonts and icon themes for the UI tests. LLVM provides `llvm-profdata` and `llvm-cov` for the
coverage build.

The bundled SDL3 needs the ALSA, PulseAudio, Wayland, X11, libudev, and D-Bus development files.
Configure stops with an error that names every SDL3 backend it cannot enable, so a build without
audio or gamepad hotplug support cannot happen by accident.

### Fedora

```bash
sudo dnf install \
  clang lld llvm cmake ninja-build ccache git pkgconf-pkg-config patch file binutils \
  glslang python3 python3-pyyaml gettext \
  gtk4-devel libadwaita-devel libsoup3-devel glib2-devel glib-networking \
  adwaita-icon-theme hicolor-icon-theme dejavu-sans-fonts \
  alsa-lib-devel dbus-devel pulseaudio-libs-devel systemd-devel \
  wayland-devel wayland-protocols-devel \
  libX11-devel libXcursor-devel libXext-devel libXfixes-devel libXi-devel libxkbcommon-devel \
  libXrandr-devel libXScrnSaver-devel libXtst-devel mesa-libGL-devel \
  vulkan-loader mesa-vulkan-drivers \
  weston xorg-x11-server-Xvfb dbus-daemon gcc-c++
```

On Fedora Atomic variants such as Silverblue, Bazzite, and Bluefin, run the command and the build
inside a [Distrobox](https://distrobox.it/) or Toolbx container of the same Fedora release. Keep the
checkout in your home directory so that the container shares it. Add `--nvidia` to
`distrobox create` on hosts with the proprietary NVIDIA driver:

```bash
distrobox create --name kyty-build --image registry.fedoraproject.org/fedora:latest
distrobox enter kyty-build
```

### Ubuntu and Debian

Ubuntu 24.04 or newer, or a Debian release with at least the library versions listed under
[Toolchain](#toolchain):

```bash
sudo apt-get install --no-install-recommends \
  clang lld llvm cmake ninja-build ccache git pkg-config patch file binutils \
  glslang-tools python3 python3-yaml gettext \
  libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev glib-networking \
  adwaita-icon-theme hicolor-icon-theme fonts-dejavu-core \
  libasound2-dev libdbus-1-dev libpulse-dev libudev-dev libwayland-dev wayland-protocols \
  libx11-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev libxkbcommon-dev libxrandr-dev \
  libxss-dev libxtst-dev libgl1-mesa-dev \
  libvulkan1 mesa-vulkan-drivers \
  weston xvfb dbus-daemon gcc g++
```

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel clang lld llvm cmake ninja ccache git pkgconf patch file binutils \
  glslang python python-yaml gettext \
  gtk4 libadwaita libsoup3 glib2 glib-networking \
  adwaita-icon-theme hicolor-icon-theme ttf-dejavu \
  alsa-lib dbus libpulse systemd-libs wayland wayland-protocols \
  libx11 libxcursor libxext libxfixes libxi libxkbcommon libxrandr libxss libxtst mesa libglvnd \
  vulkan-icd-loader vulkan-swrast \
  weston xorg-server-xvfb gcc
```

The other dependencies (SDL3, Vulkan headers, spdlog, fmt, and the rest under `3rdparty`) come with
the checkout's submodules. CMake downloads a few more at configure time (opus, OpenXR-SDK, xbyak,
zydis, zstd, ZArchive, and the FFmpeg core prebuilts), each pinned to a commit or a SHA-256
checksum. FFmpeg is linked statically, so no system FFmpeg package is needed.

## Configure, build, and install

Clone with the submodules, or initialize them in an existing checkout:

```bash
git clone --recursive https://github.com/MrG00gle/KytyPS5-GTK.git
cd KytyPS5-GTK
git submodule update --init --recursive
```

The CMake source root is the repository root. Build directories under `_Build/` are ignored by Git.

```bash
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/linux
cmake --install _Build/linux --prefix _Build/linux/install
```

The build directory holds `kyty_emulator`, `launcher/kyty_launcher`, and `updater/kyty_updater`.
The install step writes the portable layout that the release archive uses: `kyty_launcher`,
`kyty_emulator`, and `kyty_updater` side by side, the emulator assets, `LICENSE`, and the license
files of every component under `LICENSES/`. Run `_Build/linux/install/kyty_launcher` to start it
(see the [running guide](running.md)).

Useful options:

| Option | Default | Effect |
|---|---|---|
| `KYTY_BUILD_LAUNCHER` | `ON` | Builds `kyty_launcher` and `kyty_updater`. See [Emulator only](#emulator-only). |
| `KYTY_INSTALL_LAYOUT` | `portable` | `portable` installs the programs side by side. `fhs` installs `bin/`, `share/kyty/assets`, the desktop entry, the metainfo, the icons, and `share/licenses`, as the Flatpak does. Choose it on the first configure of a build directory. |
| `KYTY_ENABLE_COVERAGE` | `OFF` | Instruments the first-party code and adds the `coverage` target. See [Coverage build](#coverage-build). |
| `KYTY_PREFETCHED_SOURCES_DIR` | empty | Builds the configure-time dependencies from local copies in this directory instead of downloading them. Configure fails and names every missing dependency. The Flatpak uses it. |
| `KYTY_FLATPAK_BUILD` | `OFF` | Set only by the Flatpak manifest. It implies the `fhs` layout and leaves out `kyty_updater`, so the Flatpak's launcher offers no Updater. |

## Emulator only

With `-DKYTY_BUILD_LAUNCHER=OFF`, the build produces the emulator (`kyty_emulator`) only, without
the launcher and the Updater, and it needs no GTK, libadwaita, or libsoup packages. The test suite
then holds every test except the launcher, Updater, and UI tests:

```bash
cmake -S . -B _Build/linux-emulator -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/linux-emulator --target kyty_emulator kyty_tests
```

## Coverage build

The coverage build is a Debug build with Clang source-based coverage. It needs `llvm-profdata` and
`llvm-cov` of the same LLVM major version as Clang, which configure finds next to `clang++` or on
the `PATH` (set `KYTY_LLVM_PROFDATA` and `KYTY_LLVM_COV` to choose others). The `coverage` target
runs the whole test suite and writes the report:

```bash
cmake -S . -B _Build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKYTY_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/coverage --target all kyty_tests
cmake --build _Build/coverage --target coverage
```

The report is `_Build/coverage/coverage/html/index.html`. The [testing guide](testing.md#coverage)
describes the report files and the coverage targets that CI enforces.

## Flatpak

The [packaging guide](packaging.md) shows how to build, install, and run the Flatpak from the
manifest in `packaging/flatpak/`.
