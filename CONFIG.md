# charaWC

A small Wayland compositor built on [neuswc](src/neuswc) and
[neuwld](src/neuwld). Windows float or tile, everything is configured from one
Lua file, and a control client drives the running session from the shell.

```
~/.config/charawc/config.lua       your configuration
~/.config/charawc/built/           charawc, charactl, charabar
~/.config/charawc/log/             session and run logs
~/.config/charawc/charawc.desktop  session entry
```

## Building

```sh
./build.sh          # builds libspng, neuwld, neuswc, charaWC and charabar
./install.sh        # copies the binaries into ~/.config/charawc
```

`swc-launch` needs to be setuid root to open the DRM device and switch VTs.
`install.sh` prints the exact command; it is the only file that has to leave
your home directory.

`install.sh` also writes `built/charawc-session`, the entry point a display
manager runs. It takes the VT through `swc-launch`, starts a session bus, and
checks the configuration before anything switches. To offer charaWC at login:

```sh
sudo cp ~/.config/charawc/charawc.desktop /usr/share/wayland-sessions/
```

Run from the build tree without installing:

```sh
./run.sh            # checks the config, then starts on /dev/tty2
TTY=/dev/tty3 ./run.sh
```

Logs are written under `~/.config/charawc/log/`: `session.log` from a login,
`run.log` from `run.sh`. The previous login is kept as `session.log.previous`.

Check a configuration without starting anything:

```sh
charawc -C
charawc -C -c /path/to/config.lua
```

## The configuration file

`config.lua` returns one table. Every setting is optional; anything you leave
out keeps its default. Unknown settings are an error, so a typo is reported
with its line instead of being ignored.

```lua
return {
    mod = "logo",

    layout = {
        mode = "split",
        axis = "vertical",
        max  = 4,
    },

    appearance = {
        rings = {
            { width = 2, focused = "#d3869b", unfocused = "#3c3836" },
        },
    },

    bindings = {
        { key = "mod+Return", spawn = { "foot" } },
        { key = "mod+q", action = "close" },
    },
}
```

Reload it with `charactl reload` or the `reload` action. A configuration that
fails to parse is reported and the running one is kept.

---

## Layouts

`layout.mode` decides where a new window goes.

| Mode | Behaviour |
| --- | --- |
| `floating` | Every window opens in the middle of the active monitor. |
| `split` | All windows share the screen equally along the axis. |
| `quad` | The first two windows share the screen, the third takes a bottom band, the fourth splits that band. |

`layout.axis` is the direction of every split: `vertical` puts windows side by
side, `horizontal` stacks them. `layout.max` is how many windows a monitor
tiles before the rest open floating — up to 16, and at most 4 in `quad`.

In `split`, three windows are thirds and four are quarters — adding a window
resizes all of them, it does not subdivide the last one:

```
 2 windows      3 windows       4 windows
+-----+-----+  +---+---+---+  +--+--+--+--+
|     |     |  |   |   |   |  |  |  |  |  |
|  1  |  2  |  | 1 | 2 | 3 |  |1 |2 |3 |4 |
|     |     |  |   |   |   |  |  |  |  |  |
+-----+-----+  +---+---+---+  +--+--+--+--+
```

With `mode = "quad"` and `axis = "vertical"` the four windows form a box:

```
 1 window     2 windows     3 windows     4 windows
+---------+  +----+----+  +----+----+  +----+----+
|         |  |    |    |  |  1 |  2 |  |  1 |  2 |
|    1    |  |  1 |  2 |  +----+----+  +----+----+
|         |  |    |    |  |    3    |  |  3 |  4 |
+---------+  +----+----+  +---------+  +----+----+
```

A tiled window becomes floating with `charactl floating`, by dragging it, or
with `floating = true` in a rule. One tiled window fills the screen.

In `quad`, three windows are deliberately uneven: the third takes the whole
bottom band. Two and four windows are equal.

```lua
layout = {
    mode = "quad",       -- floating, split or quad
    axis = "vertical",   -- vertical or horizontal
    max  = 4,            -- 1 to 16
}
```

## Borders

Borders are drawn as rings around the window, innermost first. Each ring has
its own thickness and its own colour for the focused and unfocused states.

```lua
appearance = {
    rings = {
        { width = 2, focused = "#d3869b", unfocused = "#3c3836" },
        { width = 1, focused = "#1d2021", unfocused = "#1d2021" },
    },
}
```

| Setting | Values |
| --- | --- |
| `width` | `0` to `64` pixels |
| `focused` | `#RRGGBB` or `#AARRGGBB` |
| `unfocused` | `#RRGGBB` or `#AARRGGBB` |

Two rings are supported. Leaving `rings` out gives a single 2px ring.

Borders and titlebars are drawn outside a window's content, so a maximized
window leaves room for them. Turn either off to let a maximized window cover
the whole usable area instead:

```lua
appearance = {
    maximize = { borders = true, titlebar = true },
}
```

## Titlebars

Titlebars are off unless you turn them on.

```lua
appearance = {
    titlebar = {
        enabled = true,
        height = 24,
        padding = 8,
        title_position = "center",        -- left, center, right
        buttons = { "minimize", "fullscreen", "close" },
        buttons_style = "classic",        -- classic or circles
        buttons_position = "right",       -- left or right
        fullscreen_action = "maximize",   -- what the middle button does
        focused   = { background = "#1d2021", foreground = "#ebdbb2",
                      hover = "#32302f", pressed = "#504945" },
        unfocused = { background = "#141617", foreground = "#928374" },
    },
}
```

`hover` and `pressed` apply to `buttons_style = "classic"` on focused windows.
With `buttons_style = "circles"` the button colours come from `circle_colors`,
which accepts `preset = "macos"` or individual `close`, `minimize` and
`fullscreen` colours.

Turn a titlebar off for one application with `titlebar = false` in a rule.

## Window titles

An optional line of text drawn on one edge of a window, independent of the
titlebar.

```lua
appearance = {
    font = "sans-serif:size=11",
    title = {
        enabled = true,
        format = "%t",       -- %t title, %a app_id, %i window id, %w workspace
        edge = "top",        -- top, right, bottom, left
        align = "start",     -- start, center, end
        foreground = "#ebdbb2",
        background = "#1d2021",
        padding = 4,
        offset_x = 0,
        offset_y = 0,
    },
}
```

## Cursor and wallpaper

```lua
appearance = {
    cursor = { theme = "Adwaita", size = 24 },
    wallpaper = {
        path = "~/pictures/wall.png",   -- PNG, up to 8192x8192
        mode = "fill",                  -- fill, fit or center
        background = "#1d2021",         -- shown where the image does not reach
    },
}
```

Relative paths are resolved against the configuration file. The cursor theme
is also exported to clients through `XCURSOR_THEME` and `XCURSOR_SIZE`.

## Window ids

Every window gets an automatic id written as `#1`, `#2`, and so on. A rule can
give an application a name instead, and further windows of the same name are
numbered: `term`, `term:2`, `term:3`.

Commands accept any of these, and `focused` for the focused window:

```sh
charactl close term        # the first terminal
charactl close term:2      # the second one
charactl close '#7'        # by automatic id
charactl close             # the focused window
```

`#` and `:` are reserved and cannot appear in a name.

## Rules

Rules match an application's `app_id` and apply when a window announces it.
One rule per `app_id`.

```lua
rules = {
    { app_id = "foot", id = "term" },
    { app_id = "mpv", id = "video", floating = true, center = true,
      width = 1280, height = 720 },
    { app_id = "pavucontrol", floating = true, titlebar = false,
      movable = true, resizable = false },
}
```

| Setting | Meaning |
| --- | --- |
| `app_id` | Application identifier to match. Required. |
| `id` | Name for the window, used by `charactl`. |
| `x`, `y` | Position. Both together, and not with `center`. |
| `width`, `height` | Size. Both together. |
| `center` | Centre on the monitor. |
| `floating` | Keep the window out of the tiling. |
| `titlebar` | Titlebar for this application only. |
| `movable`, `resizable` | Allow dragging and resizing. Default `true`. |

## Key bindings

`mod` names the modifier that `mod+` refers to in every binding: `logo`
(also `super` or `win`), `alt`, `ctrl`, `shift`, or a combination such as
`ctrl+alt`.

A binding either starts a program or runs an action.

```lua
bindings = {
    { key = "mod+Return", spawn = { "foot" } },
    { key = "mod+d", spawn = { "fuzzel" } },

    { key = "mod+q", action = "close" },
    { key = "mod+space", action = "floating" },
    { key = "mod+f", action = "maximize" },
    { key = "mod+j", action = "focus_next" },
    { key = "mod+1", action = "workspace", args = { 1 } },
    { key = "mod+shift+1", action = "move_workspace", args = { 1 } },

    { key = "mod+t", action = "layout", value = "quad" },
    { key = "mod+b", action = "focus", window = "term" },
}
```

`args` holds the numeric arguments an action needs. `value` holds the string
argument for `layout` and `layout_axis`. `window` targets a specific window;
without it, actions apply to the focused one.

Key names come from xkbcommon: `Return`, `space`, `BackSpace`, `Tab`, `Escape`,
`F1`, `a`, `1`, and so on.

Leaving `bindings` out entirely gives a default set: `mod+Return` a terminal,
`mod+q` close, `mod+f` maximize, `mod+shift+f` fullscreen, `mod+space`
floating, `mod+m`/`mod+n` minimize and restore, `mod+c` centre, `mod+j`/`mod+k`
focus, `mod+1`..`mod+9` workspaces, `mod+shift+r` reload, `mod+shift+e` quit.

## Mouse

| Action | Binding |
| --- | --- |
| Move a window | `mod` + left drag |
| Resize a window | `mod` + right drag |

Dragging a tiled window makes it floating first. Focus follows the pointer, and
each monitor remembers the window that was last focused on it.

## Monitors and workspaces

Each monitor has its own nine workspaces, so changing one monitor's workspace
leaves the others as they were.

Monitor positions are set by connector name:

```lua
monitors = {
    { name = "DP-1", x = 0, y = 0 },
    { name = "HDMI-A-1", x = 1920, y = 0 },
}
```

`charactl list_monitors` prints the connector names of the current outputs.

## Starting programs

`exec_once` runs when the session starts. `exec` runs again on every reload.

```lua
exec_once = {
    { "wireplumber" },
    { argv = { "pipewire" }, ready_socket = "pipewire-0", stop_on_exit = true },
    { argv = { "/usr/libexec/xdg-desktop-portal", "-r" }, stop_on_exit = true },
    { argv = { "dbus-update-activation-environment", "--systemd",
               "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP" }, wait = true },
}

exec = {
    { "sh", "-c", "charactl layout split" },
}
```

A plain list of strings is a command. The table form takes extra controls, and
only in `exec_once`:

| Setting | Meaning |
| --- | --- |
| `argv` | The command. Required in the table form. |
| `wait` | Finish this command before starting the next. |
| `ready_socket` | Wait until this socket in `XDG_RUNTIME_DIR` accepts a connection. |
| `stop_on_exit` | Stop the program when charaWC exits. Required with `ready_socket`. |
| `timeout_ms` | How long to wait, 100 to 60000. Default 10000. |

Commands run in order. If one fails or times out, the rest are skipped and the
reason is logged.

## The bar

charabar is a separate program that reads the same `config.lua`. Set
`bar.enabled = true` and charaWC starts and stops it with the session.

```lua
bar = {
    enabled = true,
    position = "top",
    height = 28,
    background = "#1d2021",
    foreground = "#ebdbb2",
    accent = "#d3869b",
    modules = {
        left = { "workspaces", "window" },
        right = { "network", "volume", "clock" },
    },
    clock = { format = "%a %d %b  %H:%M", interval = 30 },
}
```

Available modules are `workspaces`, `window`, `taskbar`, `clock`, `cpu`,
`memory`, `network` and `volume`.

---

## charactl

`charactl` sends one command to the running compositor and prints the reply.
Commands marked with a window take a selector first, defaulting to the focused
window.

```sh
charactl focus term
charactl move '#4' 40 0
charactl layout quad
charactl list_windows
```

### Windows

| Command | Arguments |
| --- | --- |
| `move` | `<window> <dx> <dy>` |
| `move_absolute` | `<window> <x> <y>` |
| `resize` | `<window> <dw> <dh>` |
| `resize_absolute` | `<window> <w> <h>` |
| `teleport` | `<window> <x> <y> <w> <h>` |
| `center` | `<window>` |
| `fullscreen` | `<window>` — toggles |
| `maximize` | `<window>` — toggles |
| `minimize` | `<window>` |
| `restore` | `[window]` — without one, the most recently minimized |
| `floating` | `<window>` — toggles tiled and floating |
| `hide`, `show` | `<window>` |
| `raise`, `lower` | `<window>` |
| `close` | `<window>` |

Geometry commands apply to floating windows; a tiled window is placed by the
layout until you make it floating.

### Focus and workspaces

| Command | Arguments |
| --- | --- |
| `focus` | `<window>` |
| `focus_next`, `focus_prev` | — |
| `unfocus` | — |
| `workspace` | `<1-9>` |
| `move_workspace` | `<window> <1-9>` |

### Layout

| Command | Arguments |
| --- | --- |
| `layout` | `floating`, `split` or `quad` |
| `layout_axis` | `vertical` or `horizontal` |
| `layout_max` | `<1-16>` |

### Queries

| Command | Prints |
| --- | --- |
| `get_geometry` | `<window>` → `x y width height` |
| `get_pid` | `<window>` → process id |
| `get_title`, `get_app_id`, `get_id` | `<window>` |
| `get_focus` | id of the focused window |
| `get_workspace` | active workspace |
| `get_layout` | `mode axis max` |
| `get_screen_geometry` | active monitor's `x y width height` |
| `get_cursor_position` | `x y` |
| `list_windows` | one line per window: id, workspace, tiled or floating, geometry, app_id, title |
| `list_monitors` | one line per monitor: name, geometry, workspace |

### Session

| Command | Effect |
| --- | --- |
| `reload` | Re-read `config.lua` |
| `quit` | End the session |

---

## Screen sharing

charaWC implements `wlr-screencopy`, so screen sharing works through
`xdg-desktop-portal-wlr`. Despite the name it is a plain Wayland client and
does not require wlroots.

```lua
exec_once = {
    { argv = { "/usr/libexec/xdg-desktop-portal-wlr" }, stop_on_exit = true },
    { argv = { "/usr/libexec/xdg-desktop-portal", "-r" }, stop_on_exit = true },
}
```

`xdg-desktop-portal-gnome` cannot provide screen casting here: it drives
Mutter's private D-Bus interfaces rather than a Wayland protocol. Other
portal interfaces, such as the file chooser, can still be served by any
backend you prefer through `portals.conf`.

## Licensing

charaWC is built on neuswc and neuwld, forks of
[swc](https://github.com/michaelforney/swc) and wld by Michael Forney,
maintained in this tree. Their licences are in `src/neuswc` and `src/neuwld`.
