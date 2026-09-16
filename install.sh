#!/bin/sh
# Install charaWC into ~/.config/charawc.
#
#   ./install.sh              copy the binaries and write the session entry
#   ./install.sh --uninstall  remove them again
#
# Everything charaWC needs lives under one directory:
#
#   ~/.config/charawc/config.lua       your configuration
#   ~/.config/charawc/built/           charawc, charactl, charabar
#   ~/.config/charawc/log/             session and run logs
#   ~/.config/charawc/charawc.desktop  session entry
#
# A login manager will not read the session entry from here. To offer charaWC
# at login, copy it to /usr/share/wayland-sessions yourself:
#
#   sudo cp ~/.config/charawc/charawc.desktop /usr/share/wayland-sessions/
set -eu

ROOT=$(cd -- "$(dirname -- "$0")" && pwd -P)
CONFIG_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/charawc
BUILT=$CONFIG_DIR/built
DESKTOP=$CONFIG_DIR/charawc.desktop
SESSION=$BUILT/charawc-session

if [ "${1:-}" = "--uninstall" ]; then
	rm -rf -- "$BUILT"
	rm -f -- "$DESKTOP"
	echo "removed $BUILT and $DESKTOP"
	echo "your config.lua was left alone"
	exit 0
fi
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
	exit 0
fi

for binary in src/charawc/charawc src/charawc/charactl src/charabar/charabar; do
	if [ ! -x "$ROOT/$binary" ]; then
		echo "install.sh: $binary is missing; run ./build.sh first" >&2
		exit 1
	fi
done

mkdir -p "$BUILT" "$CONFIG_DIR/log"
install -m 755 "$ROOT/src/charawc/charawc"  "$BUILT/charawc"
install -m 755 "$ROOT/src/charawc/charactl" "$BUILT/charactl"
install -m 755 "$ROOT/src/charabar/charabar" "$BUILT/charabar"

# A login manager cannot run the compositor directly: charaWC needs swc-launch
# for the VT and DRM master, a session bus, and an environment free of the
# greeter's display variables. The session script does that, and resolves the
# binaries from its own location.
cat > "$SESSION" <<'SESSION_SCRIPT'
#!/bin/sh
# Login entry point for charaWC. Written by install.sh; a .desktop entry in
# /usr/share/wayland-sessions points a display manager at this file.
set -eu

self=$0
case $self in
	*/*) ;;
	*) self=$(command -v -- "$self") ;;
esac
built=$(cd -- "$(dirname -- "$self")" && pwd -P)
charawc=$built/charawc
# Logs live beside the configuration, not in the home directory.
logdir=$(dirname -- "$built")/log
log=$logdir/session.log
mkdir -p -- "$logdir"

# charaWC starts charabar by name.
PATH="$built:$PATH"
export PATH
export XDG_CURRENT_DESKTOP=charaWC
export XDG_SESSION_DESKTOP=charawc
export DESKTOP_SESSION=charawc
export XDG_SESSION_TYPE=wayland

# This script takes over the VT it is given. Inside a running desktop it would
# fight the compositor already there for the VT and DRM master.
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then
	echo "charawc-session is for a login manager, not a running desktop." >&2
	echo "Use run.sh in the build tree to start charaWC from here." >&2
	exit 1
fi

# A display manager can pass on variables from its greeter.
unset DISPLAY WAYLAND_DISPLAY SWC_LAUNCH_SOCKET SWC_LAUNCH_TTY

[ -f "$log" ] && mv -f -- "$log" "$log.previous"
: > "$log"
printf 'charaWC login session: %s\n' "$(date -Iseconds)" >> "$log"

if [ ! -x "$charawc" ]; then
	echo "charaWC is not installed at $charawc; run install.sh" | tee -a "$log" >&2
	exit 1
fi

# swc-launch is setuid root and lives outside the config directory. A login
# shell's PATH does not always include /usr/local/bin.
launch=$(command -v swc-launch 2>/dev/null || true)
for candidate in /usr/local/bin/swc-launch /usr/bin/swc-launch; do
	[ -n "$launch" ] && break
	[ -x "$candidate" ] && launch=$candidate
done
if [ -z "$launch" ]; then
	echo "swc-launch not found; see install.sh" | tee -a "$log" >&2
	exit 1
fi

if ! "$charawc" -C >> "$log" 2>&1; then
	echo "charaWC configuration check failed; see $log" >&2
	exit 1
fi

case ${XDG_VTNR:-} in
	''|*[!0-9]*) session_tty=$(tty) ;;
	*) session_tty=/dev/tty$XDG_VTNR ;;
esac
printf 'Login VT: %s\n' "$session_tty" >> "$log"

cd "$HOME"
exec dbus-run-session -- "$launch" -t "$session_tty" -- "$charawc" >> "$log" 2>&1
SESSION_SCRIPT
chmod 755 "$SESSION"

cat > "$DESKTOP" <<DESKTOP_ENTRY
[Desktop Entry]
Name=charaWC
Comment=charaWC Wayland compositor
Exec=$SESSION
Type=Application
DesktopNames=charaWC
DESKTOP_ENTRY
chmod 644 "$DESKTOP"

if [ ! -e "$CONFIG_DIR/config.lua" ]; then
	cat > "$CONFIG_DIR/config.lua" <<'STARTER'
-- charaWC configuration. See CONFIG.md for every setting.
return {
	mod = "logo",

	layout = {
		mode = "split",     -- floating, split or quad
		axis = "vertical",  -- windows side by side
		max  = 4,           -- further windows open floating
	},

	appearance = {
		rings = {
			{ width = 2, focused = "#d3869b", unfocused = "#3c3836" },
		},
		titlebar = { enabled = false },
	},

	bindings = {
		{ key = "mod+Return", spawn = { "foot" } },
		{ key = "mod+q", action = "close" },
		{ key = "mod+space", action = "floating" },
		{ key = "mod+shift+r", action = "reload" },
		{ key = "mod+shift+e", action = "quit" },
	},
}
STARTER
	echo "wrote a starter $CONFIG_DIR/config.lua"
fi

echo "installed into $CONFIG_DIR"
echo
echo "swc-launch must be setuid root to start a session:"
echo "  sudo install -m 4755 -o root -g root \\"
echo "    $ROOT/src/neuswc/build/launch/swc-launch /usr/local/bin/swc-launch"
echo
echo "to offer charaWC at login:"
echo "  sudo cp $DESKTOP /usr/share/wayland-sessions/"
