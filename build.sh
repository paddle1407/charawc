#!/bin/sh
# Build the charaWC stack into ./prefix.
#
#   ./build.sh          incremental
#   ./build.sh clean    wipe the build directories first
#   PNG=0 ./build.sh    no libspng; wallpapers fall back to a solid colour

set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
PREFIX="$ROOT/prefix"
SRC="$ROOT/src"
PNG=${PNG:-1}

# Keep the caller's search path so that "is spng already installed?" asks about
# the system rather than about a copy an earlier run left in ./prefix.
SYSTEM_PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${SYSTEM_PKG_CONFIG_PATH:+:$SYSTEM_PKG_CONFIG_PATH}"

# libspng is bundled only as a fallback: skip it entirely without PNG support,
# and prefer a distribution package when there is one.
SPNG=bundled
if [ "$PNG" = 0 ]; then
	SPNG=disabled
elif PKG_CONFIG_PATH=$SYSTEM_PKG_CONFIG_PATH pkg-config --exists spng 2>/dev/null; then
	SPNG=system
fi

REQUIRED="neuswc neuwld"
[ "$SPNG" = bundled ] && REQUIRED="libspng $REQUIRED"

for sub in $REQUIRED; do
	[ -e "$SRC/$sub/meson.build" ] && continue
	echo "error: src/$sub is empty -- the subprojects are git submodules." >&2
	echo "       run: git submodule update --init --recursive" >&2
	exit 1
done

[ "$1" = clean ] && rm -rf "$SRC"/neuwld/build "$SRC"/libspng/build \
	"$SRC"/neuswc/build "$SRC"/charabar/build

step() { printf '\n==> %s\n' "$1"; }

meson_configure() {
	if [ -d build ]; then
		meson setup --reconfigure build --prefix="$PREFIX" --buildtype=release "$@" >/dev/null
	else
		meson setup build --prefix="$PREFIX" --buildtype=release "$@" >/dev/null
	fi
}

case $SPNG in
system)   printf '\n==> libspng: using the version installed on this system\n' ;;
disabled) printf '\n==> libspng: skipped (PNG=0, wallpaper images unavailable)\n' ;;
bundled)
	step libspng
	cd "$SRC/libspng"
	meson_configure
	meson install -C build >/dev/null
	;;
esac

step neuwld
cd "$SRC/neuwld"
# GPU composition is required here, not an optional auto-detected extra:
# fail the build on missing dependencies instead of silently dropping GBM.
meson_configure -Ddrm=enabled -Ddrivers=auto,gbm
meson install -C build >/dev/null

step neuswc
cd "$SRC/neuswc"
meson_configure
meson install -C build >/dev/null

step charawc
cd "$SRC/charawc"
# $ORIGIN/lib comes first so that install.sh can put libswc beside the
# installed binary and the session stops depending on this checkout staying
# where it is. Nothing sits there in the build tree, so it falls through.
make PNG="$PNG" EXTRA_CPPFLAGS="-I$PREFIX/include" \
     EXTRA_LDFLAGS="-Wl,-rpath,'\$\$ORIGIN/lib' -Wl,-rpath,$PREFIX/lib" >/dev/null

step charabar
cd "$SRC/charabar"
make >/dev/null

printf '\nbuilt:\n'
printf '  %s\n' "$SRC/charawc/charawc" "$SRC/charawc/charactl" \
	"$SRC/charabar/charabar" "$SRC/neuswc/build/launch/swc-launch"
printf '\nswc-launch must be installed setuid root to start a session:\n'
printf '  sudo install -m 4755 -o root -g root \\\n'
printf '    %s /usr/local/bin/swc-launch\n' "$SRC/neuswc/build/launch/swc-launch"
printf '\nthen: ./install.sh\n'
