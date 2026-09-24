# charaWC

A small Wayland compositor built on [neuswc](src/neuswc) and
[neuwld](src/neuwld). Windows float, everything is configured from one Lua
file, and a control client drives the running session from the shell.

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

Write the starter configuration, if there is not one already:

```sh
charawc --write-config
```

`install.sh` does this for you. An existing `config.lua` is never overwritten.

## The configuration file

`config.lua` returns one table. Every setting is optional; anything you leave
out keeps its default. Unknown settings are an error, so a typo is reported
with its line instead of being ignored.

```lua
return {
    mod = "logo",

    appearance = {
        rings = {
            { width = 2, focused = "#fabd2f", unfocused = "#3c3836" },
        },
    },

    bindings = {
        { key = "mod+Return", spawn = { "foot" } },
        { key = "mod+q", action = "close" },
    },
}
```

Reload it with `charactl reload` or the `reload` action. A configuration that
fails to parse is reported and the running one is kept, so a bad edit cannot
take the session down; fix the file and reload again.

At startup there is no running configuration to fall back on, so:

| On startup | charaWC does |
| --- | --- |
| No `config.lua` | Writes the starter one shown by `--write-config`, then loads it. |
| `config.lua` fails to parse | Reports the error and starts on the built-in defaults, including the default bindings, so the session always has a working `reload` and `quit`. |
| `config.lua` parses | Uses it. |

---

## Tiling

Windows float by default. Turning tiling on makes new windows take a share of
the workspace instead, and floating stays available either way: `tile` takes one
window out of the tiling and puts it back, and a rule can keep an application
out of it for good.

Tiling is per workspace. `enabled` is only where each workspace starts;
`tile_workspace` turns it on and off for the workspace in front of you, so a
floating session can tile the one workspace that wants it and leave the rest
alone.

```lua
tiling = {
    enabled = true,
    layout = "master",    -- master, columns, rows, grid or monocle
    gaps = { inner = 6, outer = 6 },
    smart_gaps = true,    -- a workspace showing one window gets no gaps
    master = { count = 1, ratio = 55, side = "left" },
    resize_step = 40,     -- pixels a keyboard resize moves a fence
    insert = "after_focus",
}
```

`gaps` may also be a single number, which sets both.

| Setting | |
| --- | --- |
| `enabled` | Whether a workspace starts out tiling. Default `false`; `tile_workspace` changes it per workspace at runtime. |
| `layout` | The layout every workspace starts on. Changed per workspace at runtime. |
| `gaps.inner` | Between neighbouring windows. |
| `gaps.outer` | Around the edge of the workspace. |
| `smart_gaps` | Drop both when a workspace is showing one window. Default `true`. |
| `master.count` | Windows in the master group. |
| `master.ratio` | The master group's share, as a percentage from 5 to 95. |
| `master.side` | `left`, `right`, `top` or `bottom`. |
| `resize_step` | Pixels a `tile_resize_*` with no argument moves a fence. |
| `insert` | Where a new window lands: `after_focus`, `end` or `start`. |
| `drag_swaps` | Whether a mod+drag over another tiled window trades their places. Default `true`. |
| `focus_follows_relayout` | Whether a window sliding under a still pointer may take the focus. Default `false`. |

Each monitor's workspaces carry their own layout, master count and master
ratio, so two workspaces on one monitor can be arranged differently and
switching between them switches the arrangement too. Reloading puts them all
back to what this section says. Whether a workspace tiles is the session's, so
a reload leaves that where you put it.

### The layouts

```
 master             columns            rows
+------+------+    +----+----+----+   +-------------+
|      |  2   |    |    |    |    |   |      1      |
|      +------+    | 1  | 2  | 3  |   +-------------+
|  1   |  3   |    |    |    |    |   |      2      |
|      +------+    |    |    |    |   +-------------+
|      |  4   |    |    |    |    |   |      3      |
+------+------+    +----+----+----+   +-------------+

 grid               monocle
+------+------+    +-------------+
|  1   |  2   |    |             |
+------+------+    |      1      |   every window gets the whole
|  3   |  4   |    |             |   workspace, the focused one on top
+------+------+    +-------------+
```

**master** is a master group beside a stack of everything else. `master.count`
windows are in the group, `master.ratio` is its share, and `master.side` decides
which edge it takes. With nothing in the stack the masters share the whole
workspace.

**columns** and **rows** give every window a full-height column or a full-width
row.

**grid** is as square as the count allows, filling rows from the top. An
incomplete last row stretches to fill.

**monocle** gives every window the whole workspace, one on top of another.
Focusing a window brings it to the front, so `focus_next` and `focus_prev` step
through them; so do `focus_left` and the rest, which have nothing to point at in
monocle and step through the order instead, wrapping at both ends.

### Sizes

Every window keeps its own share of whatever group it lands in, so opening and
closing windows never resizes the ones that stay. Resizing acts on the fence
between two windows: growing one takes the pixels from its neighbour and leaves
everything else where it was. A window that tells charaWC it has a minimum size
is not squeezed past it -- the fence stops there instead.

`tile_equalize` puts every window on the workspace back on equal terms.

### Using it

| | |
| --- | --- |
| `tile_workspace` | Turn this workspace's tiling on or off. |
| `tile` | Take a window out of the tiling, or put it back. |
| `focus_left` `focus_right` `focus_up` `focus_down` | Focus the window that way, carrying on to the next monitor when there is none on this one. Works for floating windows too. |
| `tile_move_left` and so on | Trade places with the window that way, or send it to the next monitor. |
| `tile_resize_left` and so on | Move the fence on that side outward, by `resize_step` or by a given number of pixels. |
| `tile_promote` | Send a window to the master slot, or swap it back. |
| `tile_swap` | Trade places with the focused window. |
| `tile_equalize` | Every window back to an equal share. |
| `tile_master` `tile_columns` `tile_rows` `tile_grid` `tile_monocle` | Switch this workspace's layout. |
| `tile_layout_next` `tile_layout_prev` | Step through them. |
| `tile_master_count` | Change the master count by the given amount. |
| `tile_master_ratio` | Change the master ratio by the given percentage. |
| `get_tiling` | The layout, how many windows are tiled, the master settings, and whether the workspace tiles. |

With the mouse, on a tiled window:

- **mod + left drag** outlines the window under the pointer as it passes over;
  letting go there trades the two windows' places.
- **mod + right drag** moves the fences the window is against -- both of them
  when the drag starts near a corner.
- **dragging its own titlebar** takes it out of the tiling and leaves it under
  the pointer, so the drag carries straight on.

A tiled window's `maximize` lifts it out of the grid to fill the workspace and
drops it back into the same place afterwards. Going fullscreen does the same.
Neither disturbs the layout, and a client asking to be maximized on its own is
ignored while it is tiled -- otherwise applications that start maximized would
jump out of the grid before you saw them in it.

---

## Overview

`overview` toggles a packed view of windows on the active monitor. The default
binding is **Mod+Tab**. Windows retain their geometry and state; the compositor
scales their latest available buffers, including retained frames from hidden
workspaces and minimized windows. Every eligible window is included, shrinking
to fit without a count cap or pagination.

```lua
overview = {
    scope = "monitor", -- "monitor" or "workspace"
    gaps = { inner = 8, outer = 30 },
    include_minimized = true,
    labels = true,
},
```

`monitor` includes all workspaces on the active monitor; `workspace` includes
only that monitor's current workspace. Other monitors keep their normal view.
Outer gaps must be at least 1 pixel, leaving background space to cancel. The
layout uses the monitor's usable area so the bar remains visible. With extreme
window counts or a tiny output, labels and gaps are reduced before any window
would be omitted. If even one pixel per window cannot fit, overview does not
open.

- Hover highlights a thumbnail. Left-click selects it, switches to its workspace,
  restores it if minimized, and focuses it.
- Right-click requests that the window close. The overview stays open while the
  client handles the request.
- Mod+Tab again selects the highlighted window. Open overview, hover a window,
  then press Mod+Tab to switch to it without clicking.
- Escape or left-clicking the overview background cancels and restores the
  previous focus when that window still exists and is visible.
- Arrows or hjkl move the selection. Tab / Shift+Tab cycle; Enter selects.

Overview captures input only while the pointer is on its monitor. Other monitors
remain usable: clicks, typing, scrolling, and shortcuts work normally, and their
workspace changes leave overview open. Returning to the overview monitor resumes
selection. One overview can be open at a time; Mod+Tab on another monitor leaves
the existing overview alone.

VT switching remains available. Session lock, VT deactivation, changes to the
overview monitor, configuration reload, and `charactl` mutations targeting that
monitor end the mode. Cancelling from another monitor preserves its focus.
New windows join the layout; closed windows leave it. Minimized windows carry a
label, or a small marker when labels are disabled.

A custom bindings list replaces the defaults, so add this entry when using one:

```lua
{ key = "mod+Tab", action = "overview" },
```

## Borders

Borders are drawn as rings around the window, innermost first. Each ring has
its own thickness and its own colour for the focused and unfocused states.

```lua
appearance = {
    rings = {
        { width = 2, focused = "#fabd2f", unfocused = "#3c3836" },
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
        buttons = { "minimize", "fullscreen", "close", "pin" },
        buttons_style = "classic",        -- classic or circles
        buttons_position = "right",       -- left or right
        fullscreen_action = "maximize",   -- what the middle button does
        focused   = { background = "#1d2021", foreground = "#ebdbb2",
                      hover = "#32302f", pressed = "#504945" },
        unfocused = { background = "#141617", foreground = "#928374" },
    },
}
```

Up to four buttons, in the order given. `hover` and `pressed` apply to
`buttons_style = "classic"` on focused windows. With `buttons_style =
"circles"` the button colours come from `circle_colors`, which accepts
`preset = "macos"` or individual `close`, `minimize`, `fullscreen` and `pin`
colours.

Turn a titlebar off for one application with `titlebar = false` in a rule.

### Pinning

The `pin` button keeps a window above the others, fullscreen ones included --
a terminal pinned over a fullscreen video stays visible instead of being
buried. Click it again to let go. The button shows the state: the pin stays
lit while it is holding, rather than only on hover.

Pinning is about stacking, not workspaces: a pinned window still belongs to
the workspace it is on. It also stays below the overlay layer, so a lock
screen still covers it.

`charactl pin <window>` toggles the same state, and `action = "pin"` binds it
to a key.

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

The theme covers more than the arrow. charaWC implements `cursor-shape-v1`, so
a client that asks for a named cursor -- the I-beam over a text field, the
double arrow on a window edge -- is given that shape from your theme rather
than drawing one of its own. Every application's text cursor then looks the
same. A theme that is missing a shape falls back to the closest one it does
have, and only then to the client's own cursor.

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
    { app_id = "mpv", id = "video", tiling = false, center = true,
      width = 1280, height = 720 },
    { app_id = "pavucontrol", tiling = false, titlebar = false,
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
| `tiling` | `false` keeps the application out of the tiling, `true` puts it in whatever the default is. Its size and position above then apply, as they do to any floating window. |
| `pinned` | `true` keeps the window above every other, fullscreen ones included — the same state the pin button sets. |
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
    { key = "mod+f", action = "maximize" },
    { key = "mod+j", action = "focus_next" },
    { key = "mod+1", action = "workspace", args = { 1 } },
    { key = "mod+shift+1", action = "move_workspace", args = { 1 } },

    { key = "mod+b", action = "focus", window = "term" },
}
```

`args` holds the numeric arguments an action needs. `window` targets a
specific window; without it, actions apply to the focused one.

Key names come from xkbcommon: `Return`, `space`, `BackSpace`, `Tab`, `Escape`,
`F1`, `a`, `1`, and so on.

Leaving `bindings` out entirely gives a default set: `mod+Return` a terminal,
`mod+q` close, `mod+f` maximize, `mod+shift+f` fullscreen, `mod+m`/`mod+n`
minimize and restore, `mod+c` centre, `mod+j`/`mod+k` focus, `mod+1`..`mod+9`
workspaces, `mod+shift+r` reload, `mod+shift+e` quit.

## Mouse

| Action | Binding |
| --- | --- |
| Move a window | `mod` + left drag, or drag its titlebar |
| Resize a window | `mod` + right drag |

Both drags act on the window under the pointer, and dragging a maximized
window gives up being maximized first. A window dropped on another monitor
belongs to that monitor, and to the workspace that monitor is showing.

On a tiled window the same two drags mean something else, because a tiled
window has nowhere free to be dragged to: a left drag outlines the window under
the pointer and trades their places when you let go, and a right drag moves the
fences the window is against. Dragging a tiled window's own titlebar takes it
out of the tiling and leaves it under the pointer, so the drag carries on.
See [Tiling](#tiling).

Focus follows the pointer, and each monitor remembers the window that was last
focused on it. Focus alone does not change the stacking order:

```lua
raise_on_hover = true,
```

brings a window to the front as the pointer enters it. It is off by default, so
that passing over a stack of windows does not shuffle it. Clicking a window
raises it either way, and so does picking one from a taskbar.

## Stacking

A window raised by hand stays raised. Leaving a workspace and coming back does
not restack anything, and neither does moving a window between monitors. A
window is brought to the front when it is opened, clicked, dragged, restored
from minimized, sent to the workspace already on screen, or activated from a
taskbar or dock.

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

A window going fullscreen fills the monitor it is already on. Clients may name
a monitor of their own when they ask, but the one they name is usually just the
first monitor charaWC advertised rather than one you picked -- Unity games ask
for their "display 0" wherever their window happens to be -- so the request is
ignored by default. To take clients at their word:

```lua
fullscreen_follows_client = true,
```

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
    { "sh", "-c", "charactl restore" },
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

charabar reads its configuration once, when it starts, so charaWC restarts it
on a reload whenever anything in the `bar` section changed. A reload that
leaves the section alone leaves the running bar alone.

```lua
bar = {
    enabled = true,
    position = "top",
    height = 28,
    background = "#1d2021",
    foreground = "#ebdbb2",
    accent = "#fabd2f",
    modules = {
        left = { "workspaces", "window" },
        right = { "network", "volume", "clock" },
    },
    clock = { format = "%a %d/%m/%Y  %H:%M", interval = 30 },
}
```

Available modules are `workspaces`, `window`, `taskbar`, `clock`, `cpu`,
`memory`, `network` and `volume`.

### Bar appearance

| Key | Meaning |
| --- | --- |
| `position` | `top` or `bottom` |
| `layer` | `top` keeps the bar above windows, `bottom` below them |
| `exclusive` | Whether windows are kept out of the bar's strip |
| `height` | Bar height in pixels |
| `padding` | Space between the bar's edge and the first module |
| `spacing` | Space between modules |
| `font` | Pango font description, e.g. `"MonaspiceRn Nerd Font Regular 10"` |
| `background`, `foreground` | Bar colours |
| `accent` | Used for the active workspace and similar highlights |
| `muted` | Used for text that should recede, such as inactive entries |

### Module options

Each module takes its own table. `interval` is in seconds, from 1 to 3600.
`volume` defaults to `0`, which stops it polling and refreshes it only when
charabar is sent `SIGUSR1` -- a keybind that changes the volume can tell it
about every change, and polling would fork a `wpctl` every interval to read
a number that had not moved. Set an interval to poll instead. The other
modules have no such mode and require at least 1. `clock` without an
`interval` is refreshed as often as its format can show a difference: once
a minute for `%H:%M`, once a second for a format with seconds in it.

```lua
workspaces = { count = 9, format = "%n" },
clock      = { format = "%a %d/%m/%Y  %H:%M", interval = 30 },
memory     = { format = "mem %p%", interval = 2 },
volume     = { format = "vol %p%", format_muted = "Muted", interval = 0 },
network    = { format_online = "net", format_offline = "---", interval = 5 },
```

`%p` is the percentage a module reports; `%n` is the workspace number.

The taskbar has more, because it is the one module that has to fit an
unpredictable number of entries into whatever room the others leave:

| Key | Meaning |
| --- | --- |
| `max_length` | Characters per entry before the title is cut short |
| `scope` | `workspace`, `monitor` or `all` -- which windows are listed |
| `overflow` | `shrink`, `scroll` or `none` when there is not enough room |
| `min_width` | Entries stop narrowing here and start scrolling instead |
| `max_width` | Widest the strip may get; `0` means as wide as it can |
| `scroll_step` | Pixels the strip moves per wheel notch |

`clock.format` is a `strftime` format, so `%H:%M` is a 24-hour clock and
`%I:%M %p` a 12-hour one. Note that `%M` is the minute but `%m` is the month
number, and `%h` is the abbreviated month name, not the hour. `interval` is
how often the clock is re-read, in seconds; keep it well under a minute for a
`%M` clock, or the minute on show can lag behind by nearly that long.

---

## charactl

`charactl` sends one command to the running compositor and prints the reply.
Commands marked with a window take a selector first, defaulting to the focused
window.

```sh
charactl focus term
charactl move '#4' 40 0
charactl maximize term
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
| `hide`, `show` | `<window>` |
| `raise`, `lower` | `<window>` |
| `pin` | `<window>` — toggles staying above the others |
| `close` | `<window>` |

Geometry commands do not apply to a window that is fullscreen or maximized;
restore it first.

### Focus and workspaces

| Command | Arguments |
| --- | --- |
| `focus` | `<window>` |
| `focus_next`, `focus_prev` | — |
| `focus_left`, `focus_right`, `focus_up`, `focus_down` | — the window that way, on this monitor or the next |
| `unfocus` | — |
| `workspace` | `<1-9>` |
| `move_workspace` | `<window> <1-9>` |

### Tiling

| Command | Arguments |
| --- | --- |
| `tile_workspace` | — the active workspace's tiling on or off, taking the windows on it with it |
| `tile` | `<window>` — in or out of the tiling |
| `tile_move_left`, `tile_move_right`, `tile_move_up`, `tile_move_down` | `<window>` |
| `tile_resize_left`, `tile_resize_right`, `tile_resize_up`, `tile_resize_down` | `<window> [pixels]` — `resize_step` without one |
| `tile_promote` | `<window>` — to the master slot, or back |
| `tile_swap` | `<window>` — trades places with the focused one |
| `tile_equalize` | — |
| `tile_master`, `tile_columns`, `tile_rows`, `tile_grid`, `tile_monocle` | — |
| `tile_layout_next`, `tile_layout_prev` | — |
| `tile_master_count` | `<change>` |
| `tile_master_ratio` | `<percent>` |

### Screen

| Command | Arguments |
| --- | --- |
| `overview` | Toggle window overview on the active monitor. |
| `zoom` | `<percent>` — 100 is normal, 10 to 1000. Scales the monitor about its centre. |

Zoom is a magnifier over the whole screen: the pointer and the keyboard still
go where the windows really are, not where they are drawn. A panel or a status
bar keeps its real size, so the furniture around the desktop does not shrink
with it.

### Queries

| Command | Prints |
| --- | --- |
| `get_geometry` | `<window>` → `x y width height` |
| `get_pid` | `<window>` → process id |
| `get_title`, `get_app_id`, `get_id` | `<window>` |
| `get_focus` | id of the focused window |
| `get_workspace` | active workspace |
| `get_tiling` | active workspace's `layout`, tiled window count, `side count ratio`, and `on` or `off` |
| `get_screen_geometry` | active monitor's `x y width height` |
| `get_cursor_position` | `x y` |
| `list_windows` | one line per window: id, workspace, geometry, app_id, title |
| `list_monitors` | one line per monitor: name, geometry, workspace |

### Session

| Command | Effect |
| --- | --- |
| `reload` | Re-read `config.lua` |
| `quit` | End the session |

---

## Screen locking

charaWC implements `ext-session-lock-v1`, so any locker written for it --
`swaylock`, `waylock`, `gtklock` -- works. There is no lock command built in;
bind whichever you use:

```lua
{ key = "mod+shift+l", spawn = { "swaylock", "-f" } },
```

While the session is locked, windows are hidden, the wallpaper is painted
black, and key bindings do nothing. Switching virtual terminal
(`Ctrl+Alt+F2`) is the deliberate exception, and is the way out if a locker
crashes: a locker that dies does **not** unlock the session, which is the
point of locking it.

## Idling

`ext-idle-notify-v1` lets a program ask to be told when you have been away for
a while, which is what an idle daemon uses to lock or blank the screen:

```lua
exec_once = {
    { argv = { "swayidle", "-w",
               "timeout", "300", "swaylock -f",
               "timeout", "600", "charactl quit" } },
},
```

`idle-inhibit-v1` is honoured, so a video player that asks to keep the session
awake will stop the timer from running.

## Selections, activation and input methods

Three protocols that mostly matter by being present:

`primary-selection-v1` is the second clipboard: text you highlight can be
pasted with the middle mouse button, without copying it first.

`xdg-activation-v1` lets one program hand focus to another. Clicking a link in
a chat window brings the browser forward instead of leaving it to blink in the
taskbar. A window raised this way goes through the same path as clicking it in
charabar's taskbar, so it is un-minimized and its workspace switched to if
needed.

`text-input-v3` and `input-method-v2` are the two halves of input method
support, for typing scripts a keyboard has no keys for. Start the input method
with the session:

```lua
exec_once = {
    { argv = { "fcitx5" } },
},
```

The input method takes the keyboard while composing, so its candidate keys do
not reach the application underneath. Compositor key bindings still work.

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
