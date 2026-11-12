# Architecture

KytyPS5-GTK has two programs that users start: the emulator `kyty_emulator` and the GTK launcher
`kyty_launcher`. The launcher runs the emulator as a separate process. Release archives add a third
program, `kyty_updater`, which replaces the installed files after the launcher quits.

## Source tree

The emulator's entry point is `src/main.cpp`. `src/emulatorOptions.cpp` holds the single table of
emulator options, which both the option parser and `kyty_emulator --help` read, and the data
directory rules of the Flatpak. `src/emulator.cpp` starts the subsystems and runs the game.

Directories directly under `src`:

| Directory | Responsibility |
|---|---|
| `src/common` | Shared infrastructure of the emulator: files and ZArchive (`.zar`) mounting and packing, logging, threads and timers, the emulator configuration that the options set, trophy data, the notification text catalogs, the profiler hooks, and the Linux system layer in `common/platform`. |
| `src/graphics` | GPU emulation: the guest GPU, the Vulkan host backend, the shader recompiler, and the window and presentation code (see below). |
| `src/kernel` | The emulated PS5 kernel: guest memory and the address space, threads (pthread), event flags, event queues, semaphores, wait-on-address synchronization, and the guest file system. |
| `src/launcher` | The launcher. `src/launcher/core` (Launcher_Core) holds its logic without any GTK dependency: game scanning and metadata, library search and sorting, the settings file, the configuration fields and their validation, the emulator arguments, starting and stopping the emulator and collecting its output, trophies, additional content, cheats, compatibility data, title sounds, input mapping, save data, and ZArchive compression. `src/launcher/gtk` is the GTK 4 and libadwaita user interface on top of it, with its services for HTTP (libsoup), process watching, audio and devices (SDL), and the desktop portals. |
| `src/libs` | High-level emulation of the PS5 system libraries that games call: libc and libKernel, audio (AudioOut, AJM, Ngs2), the pad and the DualSense (including haptics and Bluetooth), video out, AGC, save data, trophies, user and system services, networking, the IME and dialogs, AvPlayer, and video decoding. |
| `src/loader` | Loading games: the ELF loader, the runtime linker and its symbol database, game patches (cheat files), system and additional content, and the guest instruction patcher with the x86-64 instruction emulator for instructions that the host CPU lacks (for example SSE4a). |
| `src/updater` | The Updater. `src/updater/core` holds its logic without any GTK dependency: the build classification, the release metadata, the update policy, the download check (size and SHA-256), the update session, and the update result. The `kyty_updater` program installs a release with rollback and reports the result to the next launcher start. |

Directories directly under `src/graphics`:

| Directory | Responsibility |
|---|---|
| `src/graphics/guest_gpu` | The PS5 (Prospero) GPU as games see it: GPU formats, tiling, and the PM4 command processor that decodes the command buffers. |
| `src/graphics/host_gpu` | The Vulkan 1.3 host backend: host memory, page and region tracking of guest memory, the memory allocator, the renderer (pipelines and their caches, render targets, images, command scheduling, and synchronization), and the built-in host shaders. |
| `src/graphics/presentation` | Showing frames: the window and the Vulkan device selection, video out, the performance and system overlays, the trophy notifications, the OpenXR headset, RenderDoc capture, and DDS images. |
| `src/graphics/shader` | The shader recompiler, which translates guest RDNA 2 shaders through its own intermediate representation into SPIR-V (`recompiler/frontend`, `recompiler/ir`, `recompiler/backend`), plus shader bindings and metadata. |

`tests/` holds the test suite, `tools/` the Python checks and the coverage tools, `packaging/` the
desktop entry, the metainfo, the icons, and the Flatpak manifest, and `3rdparty/` the bundled
dependencies.

The PS5 GPU is based on AMD RDNA 2. AMD's
[RDNA 2 Instruction Set Architecture Reference Guide](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
(document 70648) is the primary reference for shader decoding and recompilation. Keep shader changes
in line with both the RDNA 2 ISA and the Vulkan and SPIR-V validation rules.

## Layering

Launcher_Core (`src/launcher/core`) and the Updater core (`src/updater/core`) include no GTK, GLib,
or libsoup header; a CTest (`source_check_layering`) checks this. All I/O that they depend on goes
through interfaces (file system, HTTP, scheduler, environment, process runner, install file
operations), which the tests replace with fakes and the GTK layer implements. Core functions return
errors as values with a machine-readable kind, and the GTK layer turns each kind into its own
translated text.

## Process model

When you run a game, the launcher:

1. checks that no game is running and no update is being installed;
2. builds the effective configuration of the game (its own settings or the global configuration,
   the global controller settings, the input mapping, the add-ons folder, and the cheat file when
   one exists) and turns it into the emulator arguments (`BuildEmulatorArguments` in
   `src/launcher/core/emulatorArguments.cpp`), with the game path passed unchanged;
3. checks that the data directory exists or can be created and is writable, and that
   `kyty_emulator` is an executable file;
4. stops the title sound, clears the log view, and starts `kyty_emulator` as a child process with
   `posix_spawn`: the data directory as its working directory, standard input from `/dev/null`,
   and standard output and standard error connected to two pipes;
5. reads both pipes without blocking the user interface, splits the output into lines (invalid
   UTF-8 becomes U+FFFD), and appends them to the log view in batches, within a second of the write;
6. while the game runs, shows the running indicator on it, disables **Run**, and defers update
   installation.

**Stop** sends SIGTERM. If the emulator is still running 5 seconds later, the launcher sends SIGKILL.
When the emulator exits, the launcher adds the last unterminated line to the log, and it reports a
non-zero exit code or the name of the terminating signal unless you stopped the game yourself. When
the launcher quits while a game runs, it asks first and then stops the game the same way.

The emulator can also run on its own. It parses its options (`src/emulatorOptions.cpp`), prints
the usage text and exits with status 1 for any invalid option, loads the game, and writes its
output to standard output and standard error.

## Data directory

The data directory holds the files that the emulator and the launcher write while games run:

| Entry | Contents |
|---|---|
| `_SaveData` | Save data, one directory per title ID (`_SaveData/<TITLE_ID>`). **Remove save data…** in the game menu deletes a game's directory. |
| `_Trophies` | Trophy unlock records, one directory per title ID (`_Trophies/<TITLE_ID>/trophies_<user ID>_<service label>.json`). The trophy viewer reads them. |
| `_Patches` | Cheat files in the ETAHen JSON format, one per game (`_Patches/<TITLE_ID>.json`). The cheats window writes them, and the launcher passes them with `--game-patch`. |
| `_kyty.txt` | The emulator's log, which includes the game's printf output, when the log goes to a file (`printf_direction` is `File` and `printf_output_file` keeps its default). |
| `_Shaders` | Shader logs, when they go to a file (`shader_log_direction` is `File` and `shader_log_folder` keeps its default). |
| `_Buffers` | Command buffer dumps, when they are enabled (`command_buffer_dump_folder` keeps its default). |
| `_PipelineCache` | The Vulkan driver's pipeline cache, one file per title ID (`_PipelineCache/<TITLE_ID>.bin`), so that pipelines compile faster the next time. |
| `_RenderDoc` | RenderDoc captures (`_RenderDoc/kyty_<number>`), when RenderDoc capture is on (`--rd`). |
| `compatibility_cache.json` | The launcher's cached copy of the upstream KytyPS5 compatibility list. |

Locations:

- **Outside the Flatpak**, the data directory is the directory that contains `kyty_emulator`, and the
  launcher starts the emulator there. A release archive therefore keeps its data next to the
  programs, so its folder must be writable. When you start `kyty_emulator` yourself, the relative
  paths above are relative to your working directory.
- **Inside the Flatpak**, the data directory is `$XDG_DATA_HOME/Kyty` in the sandbox, which is
  `~/.var/app/io.github.MrG00gle.KytyPS5_GTK/data/Kyty` on the host. The launcher and the emulator
  both use it, including `_PipelineCache` and `_RenderDoc`. The emulator creates it, changes into
  it, and resolves the relative log, shader, and dump paths against it, also when it is started with
  `flatpak run --command=kyty_emulator`. It turns `--game`, `--game-patch`, and `--addcont-dir` into
  absolute paths against the directory it was started from first. If the directory cannot be
  created or used, the emulator stops with an error that names it.

The emulator's own assets (the trophy unlock sound and the touch cursor images) are installed with
it: under `assets/` next to `kyty_emulator` in the release archive, and under
`/app/share/kyty/assets` in the Flatpak. The [configuration reference](configuration.md) shows where
the settings file is.
