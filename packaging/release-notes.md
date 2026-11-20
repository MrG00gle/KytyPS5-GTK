<!--
Release notes for the next release. Write them below this comment, in Markdown, and commit them to
dev before dev is merged into main. Replace any notes from an earlier release: all text outside
this comment becomes the release's text. The release job uses that text when it differs from the
text that this file had at the previous release. Otherwise it lists the commit subjects since the
previous release by area. Notes are used for one release only, so they can stay in the file until
you write the next ones. See "Release notes" in docs/packaging.md.
-->

This release fixes two details of the launcher's input mapping editor and makes CI faster and its checks stricter. The emulator itself has not changed since the previous release, [KytyPS5-GTK-2026-11-18-5bce1de](https://github.com/MrG00gle/KytyPS5-GTK/releases/tag/KytyPS5-GTK-2026-11-18-5bce1de).

## Launcher

- The input mapping editor now treats the right Shift, Ctrl, Alt, and GUI (Super) keys as the left ones, as the emulator does. When you bind one of them, a control that uses the same key on the other side loses its binding. Before, the editor could show two bindings where only one worked in game.
- The editor reads the mouse sensitivity the way the emulator does. A value written in hexadecimal, such as `0x2`, now reads as 2.0 instead of 0.1.

## For contributors

- CI is faster. Full runs on `dev` took 72 to 97 minutes before these changes. Once the build caches are filled, they take about a third of that.
- Every job has a time limit, so a stuck step no longer runs for six hours, and apt retries failed downloads more often.
- The run summary shows the environment of each build job, such as the OS and the Mesa and GTK versions.
- Every GitHub Action is pinned to an exact commit, Dependabot proposes updates once a week, and checkouts no longer store the access token in the git config.
- actionlint, a linter for GitHub Actions workflows, checks every workflow file, and the workflow structure check catches more mistakes.
- Releases get notes: hand-written ones like these, or otherwise a list of the commits since the previous release.
- Smaller fixes to the documentation link checks, the coverage report filter, one translator note, and the guides.
