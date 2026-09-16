# charaWC

A small Wayland compositor built on [neuswc](https://github.com/paddle1407/neuswc)
and [neuwld](https://github.com/paddle1407/neuwld). Windows float or tile,
everything is configured from one Lua file, and a control client drives the
running session from the shell.

See [CONFIG.md](CONFIG.md) for configuration and installation.

## Getting the source

The three subprojects under `src/` are git submodules, so clone recursively:

```sh
git clone --recurse-submodules https://github.com/paddle1407/charawc.git
cd charawc
```

Already cloned without them? Fetch them after the fact:

```sh
git submodule update --init --recursive
```

| path | what it is |
| --- | --- |
| `src/charawc` | the compositor and `charactl`, its control client |
| `src/charabar` | the status bar |
| `src/neuswc` | fork of [swc](https://github.com/michaelforney/swc), the compositor library |
| `src/neuwld` | fork of [wld](https://github.com/michaelforney/wld), the drawing library |
| `src/libspng` | [libspng](https://github.com/randy408/libspng), for PNG wallpapers — optional, see below |

## Building

```sh
./build.sh          # builds libspng, neuwld, neuswc, charaWC and charabar
./install.sh        # copies the binaries into ~/.config/charawc
```

Everything is staged into `./prefix`; nothing is written outside this tree or
`~/.config/charawc`. The one exception is `swc-launch`, which must be setuid
root to open the DRM device and switch VTs — `build.sh` prints the command.

To run straight from the build tree on a spare VT, without installing:

```sh
./run.sh
```

### Dependencies

meson, ninja, `wayland-scanner`, wayland-protocols, Lua 5.2+, cairo,
pangocairo, libdrm, libxkbcommon, libxcursor and the Mesa GBM/EGL/GLES2
libraries. libspng is optional; everything else is required.

### Wallpapers, and doing without libspng

libspng decodes PNG wallpapers. It is the only thing it is used for, so there
are three ways to get it, and you may already be done:

- **Your distribution has it.** `build.sh` detects an installed `spng` through
  pkg-config and uses it, leaving the bundled copy alone. Nothing to do.
- **It doesn't, and you want PNG wallpapers.** The bundled submodule is built
  automatically. Also nothing to do.
- **You don't want the dependency at all:**

  ```sh
  PNG=0 ./build.sh
  ```

  libspng is then neither cloned nor built, and you can skip it when fetching
  the submodules:

  ```sh
  git submodule update --init src/neuswc src/neuwld
  ```

A `PNG=0` build still draws a background — it just cannot decode an image, so
`appearance.wallpaper.background` is used on its own:

```lua
appearance.wallpaper.background = 0xff1d2021
```

If a `config.lua` sets `appearance.wallpaper.path` on such a build, the path is
ignored with a warning in the log rather than being treated as an error, so the
session still starts.

## Staying up to date

Submodules are pinned to a known-good commit, so `git pull` alone can leave
them behind:

```sh
git pull --recurse-submodules
```

## License

charaWC is MIT licensed; see [LICENSE](LICENSE). The subprojects keep their own
licenses — neuswc and neuwld are MIT, Copyright (c) Michael Forney; libspng
is BSD 2-Clause, Copyright (c) Randy.
