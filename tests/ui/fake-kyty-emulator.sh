#!/bin/sh
# The Emulator of the UI tests (task 22.1; phase 7 plan D16). Each case copies this script into its
# temporary tree as app/kyty_emulator, the launcher directory of its LauncherContext, so the
# launcher finds it as its Emulator and the Data_Directory is that tree's app/.
# - Without arguments (the launcher's version probe) it prints a version banner the way the
#   Emulator does and exits 0.
# - With arguments (a game run) it writes them NUL-separated to $KYTY_UI_ARGS_FILE when that is set
#   (overwritten per run), ignores them, and execs the fake_emulator test helper
#   ($KYTY_UI_FAKE_EMULATOR) with $KYTY_UI_FAKE_ARGS, split into words on purpose (default
#   "--stdout ready --exit 0"; for example "--stdout ready --sleep-ms 60000 --ignore-term").
set -u
if [ "$#" -eq 0 ]; then
	printf 'exe_name: kyty_emulator\nKytyPS5 fake emulator (UI tests)\n'
	exit 0
fi
if [ -n "${KYTY_UI_ARGS_FILE:-}" ]; then
	printf '%s\0' "$@" > "$KYTY_UI_ARGS_FILE"
fi
if [ -z "${KYTY_UI_FAKE_EMULATOR:-}" ]; then
	echo "fake-kyty-emulator.sh: KYTY_UI_FAKE_EMULATOR is not set" >&2
	exit 127
fi
# shellcheck disable=SC2086
exec "$KYTY_UI_FAKE_EMULATOR" ${KYTY_UI_FAKE_ARGS:---stdout ready --exit 0}
