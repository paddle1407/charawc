#!/bin/sh
# Start charaWC from this build tree on a spare VT.
#
#   ./run.sh                 check the config, then launch on $TTY
#   ./run.sh -c other.lua    use another configuration file
#
# The configuration is checked before the VT switches, so a typo never leaves
# you on a blank console. Output goes to ~/.config/charawc/log/run.log.
set -eu

ROOT=$(cd -- "$(dirname -- "$0")" && pwd -P)
TTY=${TTY:-/dev/tty2}
# Logs live beside the configuration, not in the home directory.
LOGDIR=${XDG_CONFIG_HOME:-$HOME/.config}/charawc/log
LOG=$LOGDIR/run.log
mkdir -p -- "$LOGDIR"

CHARAWC=$ROOT/src/charawc/charawc
# charaWC launches charabar by name, so the build tree goes on PATH.
PATH=$ROOT/src/charawc:$ROOT/src/charabar:$PATH
export PATH

if [ ! -x "$CHARAWC" ]; then
	echo "run.sh: $CHARAWC is missing; run ./build.sh first" >&2
	exit 1
fi
if ! command -v swc-launch >/dev/null; then
	echo "run.sh: swc-launch is not on PATH; see ./build.sh" >&2
	exit 1
fi

# Keep the usual home directory for charaWC and everything it starts.
cd "$HOME"

if ! "$CHARAWC" -C "$@" >"$LOG" 2>&1; then
	cat "$LOG" >&2
	echo "run.sh: configuration check failed; the VT was not switched." >&2
	exit 1
fi
cat "$LOG"

# The pipe into tee would otherwise hide the exit status behind tee's own,
# so set -e never fired and run.sh reported success for a session that never
# started.
status_file=$(mktemp)
trap 'rm -f -- "$status_file"' EXIT INT TERM
{ swc-launch -t "$TTY" -- "$CHARAWC" "$@" 2>&1; echo $? >"$status_file"; } |
	tee -a "$LOG"
status=$(cat -- "$status_file" 2>/dev/null || echo 1)
[ -n "$status" ] || status=1
if [ "$status" -ne 0 ]; then
	echo "run.sh: charaWC exited with status $status; see $LOG" >&2
fi
exit "$status"
