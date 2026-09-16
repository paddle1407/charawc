#!/bin/sh
# Build the charaWC stack into ./prefix.
#
#   ./build.sh          incremental
#   ./build.sh clean    wipe the build directories first

set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
PREFIX="$ROOT/prefix"
SRC="$ROOT/src"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

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

step libspng
cd "$SRC/libspng"
meson_configure
meson install -C build >/dev/null

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
make EXTRA_CPPFLAGS="-I$PREFIX/include" \
     EXTRA_LDFLAGS="-Wl,-rpath,$PREFIX/lib" >/dev/null

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
