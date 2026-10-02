#!/bin/sh
# Explicit opt-in checks: desktop build, regression tests and backend builds.
set -eu
ROOT=$(cd -- "$(dirname -- "$0")" && pwd -P)
cd "$ROOT"
export PKG_CONFIG_PATH="$ROOT/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
./build.sh
make -C src/charawc test
meson test -C src/neuwld/build --print-errorlogs
meson test -C src/neuswc/build --print-errorlogs

mkdir -p temp/check
configure() {
	build=$1
	source=$2
	shift 2
	if [ -d "$build/meson-private" ]; then
		meson setup --reconfigure "$build" "$source" "$@"
	else
		meson setup "$build" "$source" "$@"
	fi
}
configure temp/check/neuwld-cpu src/neuwld \
    -Ddrm=disabled -Dwayland=disabled -Ddoxygen=disabled -Dc_std=c23,c2x
meson compile -C temp/check/neuwld-cpu
configure temp/check/neuswc-fb src/neuswc -Dvideo=fb -Dxwayland=disabled
meson compile -C temp/check/neuswc-fb
