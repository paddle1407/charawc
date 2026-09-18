# charaWC

A small Wayland compositor for Linux, built on
[neuswc](https://github.com/paddle1407/neuswc) and
[neuwld](https://github.com/paddle1407/neuwld).

- Windows float, and are moved, resized and maximized from the keyboard or the mouse.
- One Lua configuration file, reloaded in place with Super+Shift+R.
- `charactl`, a control client that drives the running session from the shell.
- `charabar`, a status bar that reads the same configuration file.
- Screen locking, idle notification, primary selection, themed cursor shapes,
  window activation and input methods, so ordinary desktop programs work.

Configuration is documented in [CONFIG.md](CONFIG.md).

## Dependencies

Build tools: a C11 compiler, `make`, `meson`, `ninja`, `pkg-config`,
`wayland-scanner` and `wayland-protocols`.

Libraries:

```
wayland  pixman  libxkbcommon  libdrm  libinput  libudev
libgbm  libEGL  libGLESv2  libXcursor  fontconfig  freetype
cairo  pangocairo  lua >= 5.2
```

On Void Linux:

```sh
xbps-install base-devel meson ninja pkg-config wayland-devel \
    wayland-protocols pixman-devel libxkbcommon-devel libdrm-devel \
    libinput-devel eudev-libudev-devel libgbm-devel libglvnd-devel \
    libXcursor-devel fontconfig-devel freetype-devel cairo-devel \
    pango-devel lua52-devel
```

At runtime charaWC also needs `elogind` or `systemd` to provide
`XDG_RUNTIME_DIR`, `xkeyboard-config` for keymaps, and a terminal emulator —
the starter configuration binds Super+Return to [foot](https://codeberg.org/dnkl/foot).

Optional: `dbus` for desktop portals and a polkit agent, PipeWire for audio,
`xorg-server-xwayland` for X11 clients, and libspng for PNG wallpapers.

## Install

```sh
git clone --recurse-submodules https://github.com/paddle1407/charawc.git
cd charawc
./build.sh
```

`swc-launch` opens the DRM device and switches VTs, so it must be setuid root:

```sh
sudo install -m 4755 -o root -g root \
    src/neuswc/build/launch/swc-launch /usr/local/bin/swc-launch
```

Then install the binaries and session entry into `~/.config/charawc`:

```sh
./install.sh
```

To offer charaWC at login, copy the session entry where your display manager
looks for it:

```sh
sudo cp ~/.config/charawc/charawc.desktop /usr/share/wayland-sessions/
```

`build.sh` stages everything into `./prefix`. Nothing is written outside this
tree and `~/.config/charawc`, apart from the two commands above.

## Running

Log out and pick charaWC from your display manager, or start it from the build
tree on a spare VT without installing:

```sh
./run.sh                 # /dev/tty2 by default
TTY=/dev/tty3 ./run.sh
```

The configuration is checked before the VT switches, so a mistake in
`config.lua` never leaves you on a blank console. Logs are written to
`~/.config/charawc/log/`.

A first run writes a starter `~/.config/charawc/config.lua`. Check it without
starting a session with `charawc -C`.

## Building without libspng

libspng decodes PNG wallpapers and nothing else. `build.sh` uses a system
`spng` when pkg-config finds one and the bundled submodule otherwise, so
usually there is nothing to do. To drop it entirely:

```sh
PNG=0 ./build.sh
```

`appearance.wallpaper.background` is then used on its own, and a configured
wallpaper path is ignored with a warning rather than failing the session.

## Repository layout

| path | what it is |
| --- | --- |
| `src/charawc` | the compositor and `charactl` |
| `src/charabar` | the status bar |
| `src/neuswc` | fork of [swc](https://github.com/michaelforney/swc), the compositor library |
| `src/neuwld` | fork of [wld](https://github.com/michaelforney/wld), the drawing library |
| `src/libspng` | [libspng](https://github.com/randy408/libspng), optional |

The subprojects are git submodules pinned to known-good commits, so update with
`git pull --recurse-submodules`.

## License

charaWC is MIT licensed; see [LICENSE](LICENSE). The subprojects keep their own
licenses — neuswc and neuwld are MIT, Copyright (c) Michael Forney; libspng is
BSD 2-Clause, Copyright (c) Randy.
