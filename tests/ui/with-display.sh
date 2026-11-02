#!/bin/bash
# Runs a command inside a private, headless desktop session (task 22.1; Requirement 16.6; phase 7
# plan D7):
#   with-display.sh [--portal <executable>] <wayland|x11> -- <command> [<argument>...]
# - A temporary tree (mktemp -d, deleted at the end) holds a 0700 XDG_RUNTIME_DIR and the
#   XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_CACHE_HOME, and HOME of the session.
# - A private dbus-daemon whose configuration has no service directories, so nothing on the host
#   is activated (no xdg-desktop-portal, no GVfs, no accessibility bus launcher).
# - With --portal, the fake org.freedesktop.portal.Desktop (tests/ui/portal/FakePortal.cpp); the
#   command starts after the portal printed its ready line.
# - weston --backend=headless (wayland) or Xvfb (x11) at 1280x800.
# - The command runs in the foreground with SIGINT and SIGTERM at their default actions, with
#   WAYLAND_DISPLAY or DISPLAY, GDK_BACKEND, GSETTINGS_BACKEND=memory, GIO_USE_VFS=local,
#   GIO_USE_VOLUME_MONITOR=unix, GIO_USE_PROXY_RESOLVER=dummy, GTK_A11Y=test (GTK's test
#   accessibility backend: no accessibility bus is needed), GSK_RENDERER=cairo,
#   SDL_AUDIO_DRIVER=dummy, NO_AT_BRIDGE=1, and LANG=C.UTF-8 (LC_ALL and LANGUAGE unset). Every other
#   variable passes through (LLVM_PROFILE_FILE in coverage builds).
# The exit status is the command's. Only a missing dbus-daemon, weston, or Xvfb executable gives 77
# (the skip code of the ui tests; CI counts a skip as a failure); a server that exists but does not
# start within 10 s is an error (exit 1, its log on stderr). KYTY_UI_DBUS_DAEMON, KYTY_UI_WESTON, and
# KYTY_UI_XVFB name other executables (the harness self-test uses them).
set -u

usage() {
	echo "usage: with-display.sh [--portal <executable>] wayland|x11 -- <command> [<argument>...]" >&2
	exit 2
}

portal=""
if [ "${1:-}" = "--portal" ]; then
	[ "$#" -ge 2 ] || usage
	portal=$2
	shift 2
fi
backend=${1:-}
case "$backend" in
	wayland | x11) ;;
	*) usage ;;
esac
shift
[ "${1:-}" = "--" ] || usage
shift
[ "$#" -gt 0 ] || usage

dbus_exe=${KYTY_UI_DBUS_DAEMON:-dbus-daemon}
weston_exe=${KYTY_UI_WESTON:-weston}
xvfb_exe=${KYTY_UI_XVFB:-Xvfb}

missing() {
	echo "with-display.sh: $1 not found ($2); skipping with exit 77" >&2
	exit 77
}
command -v "$dbus_exe" > /dev/null 2>&1 || missing dbus-daemon "$dbus_exe"
if [ "$backend" = wayland ]; then
	command -v "$weston_exe" > /dev/null 2>&1 || missing weston "$weston_exe"
else
	command -v "$xvfb_exe" > /dev/null 2>&1 || missing Xvfb "$xvfb_exe"
fi
if [ -n "$portal" ] && [ ! -x "$portal" ]; then
	echo "with-display.sh: the portal $portal is not an executable file" >&2
	exit 1
fi

root=$(mktemp -d "${TMPDIR:-/tmp}/kyty-ui-XXXXXX") || {
	echo "with-display.sh: cannot create a temporary directory" >&2
	exit 1
}
pids=()
cleanup() {
	local index pid waited
	for ((index = ${#pids[@]} - 1; index >= 0; index--)); do
		pid=${pids[index]}
		kill -TERM "$pid" 2> /dev/null || continue
		waited=0
		while kill -0 "$pid" 2> /dev/null && [ "$waited" -lt 100 ]; do
			sleep 0.05
			waited=$((waited + 1))
		done
		kill -KILL "$pid" 2> /dev/null
		wait "$pid" 2> /dev/null
	done
	rm -rf "$root"
}
trap cleanup EXIT

# fail <what> <log>: a server that exists did not start.
fail() {
	echo "with-display.sh: $1 did not start (it exited, or was not ready within 10 s); its log:" >&2
	cat "$2" >&2 2> /dev/null
	exit 1
}

# wait_until <pid> <test>...: polls the test every 50 ms for 10 s while pid lives.
wait_until() {
	local pid=$1 tries=0
	shift
	while [ "$tries" -lt 200 ]; do
		"$@" && return 0
		kill -0 "$pid" 2> /dev/null || {
			"$@"
			return
		}
		sleep 0.05
		tries=$((tries + 1))
	done
	return 1
}

mkdir -m 0700 "$root/runtime" && mkdir "$root/config" "$root/data" "$root/cache" "$root/home" || exit 1
export XDG_RUNTIME_DIR="$root/runtime" XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data"
export XDG_CACHE_HOME="$root/cache" HOME="$root/home"
unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS GDK_BACKEND LC_ALL LANGUAGE

cat > "$root/dbus.conf" << EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir=$root/runtime</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
"$dbus_exe" --config-file="$root/dbus.conf" --nofork --nopidfile --print-address=3 \
	3> "$root/dbus-address" > "$root/dbus.log" 2>&1 < /dev/null &
pids+=("$!")
wait_until "$!" test -s "$root/dbus-address" || fail dbus-daemon "$root/dbus.log"
DBUS_SESSION_BUS_ADDRESS=$(head -n 1 "$root/dbus-address")
export DBUS_SESSION_BUS_ADDRESS

if [ -n "$portal" ]; then
	"$portal" > "$root/portal.log" 2>&1 < /dev/null &
	pids+=("$!")
	wait_until "$!" grep -q '^ui_fake_portal: ready' "$root/portal.log" || fail "the portal $portal" "$root/portal.log"
fi

if [ "$backend" = wayland ]; then
	socket=kyty-ui-$$
	"$weston_exe" --backend=headless --socket="$socket" --width=1280 --height=800 --idle-time=0 \
		> "$root/weston.log" 2>&1 < /dev/null &
	pids+=("$!")
	wait_until "$!" test -S "$XDG_RUNTIME_DIR/$socket" || fail "weston ($weston_exe)" "$root/weston.log"
	export GDK_BACKEND=wayland WAYLAND_DISPLAY="$socket"
else
	"$xvfb_exe" -displayfd 4 -screen 0 1280x800x24 -nolisten tcp \
		4> "$root/xvfb-display" > "$root/xvfb.log" 2>&1 < /dev/null &
	pids+=("$!")
	wait_until "$!" test -s "$root/xvfb-display" || fail "Xvfb ($xvfb_exe)" "$root/xvfb.log"
	export GDK_BACKEND=x11 DISPLAY=":$(head -n 1 "$root/xvfb-display")"
fi

export GSETTINGS_BACKEND=memory GIO_USE_VFS=local GIO_USE_VOLUME_MONITOR=unix GIO_USE_PROXY_RESOLVER=dummy
export GTK_A11Y=test GSK_RENDERER=cairo SDL_AUDIO_DRIVER=dummy NO_AT_BRIDGE=1 LANG=C.UTF-8

(
	trap - INT TERM
	exec "$@"
)
status=$?
exit "$status"
