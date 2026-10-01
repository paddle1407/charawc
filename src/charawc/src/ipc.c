#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"

/* Command table, in enum cmd order. */
const struct command commands[cmd_last] = {
	[cmd_move]             = { "move", cmd_move, 2, true, "<window> <dx> <dy>" },
	[cmd_move_absolute]    = { "move_absolute", cmd_move_absolute, 2, true, "<window> <x> <y>" },
	[cmd_resize]           = { "resize", cmd_resize, 2, true, "<window> <dw> <dh>" },
	[cmd_resize_absolute]  = { "resize_absolute", cmd_resize_absolute, 2, true, "<window> <w> <h>" },
	[cmd_teleport]         = { "teleport", cmd_teleport, 4, true, "<window> <x> <y> <w> <h>" },
	[cmd_center]           = { "center", cmd_center, 0, true, "<window>" },
	[cmd_fullscreen]       = { "fullscreen", cmd_fullscreen, 0, true, "<window>" },
	[cmd_maximize]         = { "maximize", cmd_maximize, 0, true, "<window>" },
	[cmd_minimize]         = { "minimize", cmd_minimize, 0, true, "<window>" },
	[cmd_restore]          = { "restore", cmd_restore, 0, true, "[window]" },
	[cmd_hide]             = { "hide", cmd_hide, 0, true, "<window>" },
	[cmd_show]             = { "show", cmd_show, 0, true, "<window>" },
	[cmd_raise]            = { "raise", cmd_raise, 0, true, "<window>" },
	[cmd_lower]            = { "lower", cmd_lower, 0, true, "<window>" },
	[cmd_pin]              = { "pin", cmd_pin, 0, true, "<window>" },
	[cmd_close]            = { "close", cmd_close, 0, true, "<window>" },
	[cmd_focus]            = { "focus", cmd_focus, 0, true, "<window>" },
	[cmd_focus_next]       = { "focus_next", cmd_focus_next, 0, false, "" },
	[cmd_focus_prev]       = { "focus_prev", cmd_focus_prev, 0, false, "" },
	[cmd_unfocus]          = { "unfocus", cmd_unfocus, 0, false, "" },
	[cmd_workspace]        = { "workspace", cmd_workspace, 1, false, "<1-9>" },
	[cmd_move_workspace]   = { "move_workspace", cmd_move_workspace, 1, true, "<window> <1-9>" },
	[cmd_tile]             = { "tile", cmd_tile, 0, true, "<window>" },
	[cmd_tile_workspace]   = { "tile_workspace", cmd_tile_workspace, 0, false, "" },
	[cmd_tile_promote]     = { "tile_promote", cmd_tile_promote, 0, true, "<window>" },
	[cmd_tile_swap]        = { "tile_swap", cmd_tile_swap, 0, true, "<window>" },
	[cmd_tile_equalize]    = { "tile_equalize", cmd_tile_equalize, 0, false, "" },
	[cmd_focus_left]       = { "focus_left", cmd_focus_left, 0, false, "" },
	[cmd_focus_right]      = { "focus_right", cmd_focus_right, 0, false, "" },
	[cmd_focus_up]         = { "focus_up", cmd_focus_up, 0, false, "" },
	[cmd_focus_down]       = { "focus_down", cmd_focus_down, 0, false, "" },
	[cmd_tile_move_left]   = { "tile_move_left", cmd_tile_move_left, 0, true, "<window>" },
	[cmd_tile_move_right]  = { "tile_move_right", cmd_tile_move_right, 0, true, "<window>" },
	[cmd_tile_move_up]     = { "tile_move_up", cmd_tile_move_up, 0, true, "<window>" },
	[cmd_tile_move_down]   = { "tile_move_down", cmd_tile_move_down, 0, true, "<window>" },
	[cmd_tile_resize_left]  = { "tile_resize_left", cmd_tile_resize_left, 0, true, "<window> [pixels]", 1 },
	[cmd_tile_resize_right] = { "tile_resize_right", cmd_tile_resize_right, 0, true, "<window> [pixels]", 1 },
	[cmd_tile_resize_up]    = { "tile_resize_up", cmd_tile_resize_up, 0, true, "<window> [pixels]", 1 },
	[cmd_tile_resize_down]  = { "tile_resize_down", cmd_tile_resize_down, 0, true, "<window> [pixels]", 1 },
	[cmd_tile_master]      = { "tile_master", cmd_tile_master, 0, false, "" },
	[cmd_tile_columns]     = { "tile_columns", cmd_tile_columns, 0, false, "" },
	[cmd_tile_rows]        = { "tile_rows", cmd_tile_rows, 0, false, "" },
	[cmd_tile_grid]        = { "tile_grid", cmd_tile_grid, 0, false, "" },
	[cmd_tile_monocle]     = { "tile_monocle", cmd_tile_monocle, 0, false, "" },
	[cmd_tile_layout_next] = { "tile_layout_next", cmd_tile_layout_next, 0, false, "" },
	[cmd_tile_layout_prev] = { "tile_layout_prev", cmd_tile_layout_prev, 0, false, "" },
	[cmd_tile_master_count] = { "tile_master_count", cmd_tile_master_count, 1, false, "<change>" },
	[cmd_tile_master_ratio] = { "tile_master_ratio", cmd_tile_master_ratio, 1, false, "<percent>" },
	[cmd_overview]         = { "overview", cmd_overview, 0, false, "" },
	[cmd_zoom]             = { "zoom", cmd_zoom, 1, false, "<percent>" },
	[cmd_get_tiling]       = { "get_tiling", cmd_get_tiling, 0, false, "" },
	[cmd_get_geometry]     = { "get_geometry", cmd_get_geometry, 0, true, "<window>" },
	[cmd_get_pid]          = { "get_pid", cmd_get_pid, 0, true, "<window>" },
	[cmd_get_title]        = { "get_title", cmd_get_title, 0, true, "<window>" },
	[cmd_get_app_id]       = { "get_app_id", cmd_get_app_id, 0, true, "<window>" },
	[cmd_get_id]           = { "get_id", cmd_get_id, 0, true, "<window>" },
	[cmd_get_focus]        = { "get_focus", cmd_get_focus, 0, false, "" },
	[cmd_get_workspace]    = { "get_workspace", cmd_get_workspace, 0, false, "" },
	[cmd_get_screen_geometry] = { "get_screen_geometry", cmd_get_screen_geometry, 0, false, "" },
	[cmd_get_cursor_position] = { "get_cursor_position", cmd_get_cursor_position, 0, false, "" },
	[cmd_list_windows]     = { "list_windows", cmd_list_windows, 0, false, "" },
	[cmd_list_monitors]    = { "list_monitors", cmd_list_monitors, 0, false, "" },
	[cmd_reload]           = { "reload", cmd_reload, 0, false, "" },
	[cmd_quit]             = { "quit", cmd_quit, 0, false, "" },
};

/* Not zero-initialised: vsnprintf terminates the message, and clearing all
 * MAXSIZE bytes of it on every reply was most of what a reply cost. */
static status
ok(const char *fmt, ...)
{
	status s;
	s.ok = true;
	va_list list;
	va_start(list, fmt);
	vsnprintf(s.msg, sizeof(s.msg), fmt, list);
	va_end(list);
	return s;
}

static status
fail(const char *fmt, ...)
{
	status s;
	s.ok = false;
	va_list list;
	va_start(list, fmt);
	vsnprintf(s.msg, sizeof(s.msg), fmt, list);
	va_end(list);
	return s;
}

static bool
number(const char *text, int32_t min, int32_t max, int32_t *out)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || *end || value < min || value > max)
		return false;
	*out = (int32_t)value;
	return true;
}

/*
 * The direction and the layout a command names.
 *
 * Each of these is its own command -- an action carries numbers and a window
 * selector and nothing else -- so the argument is in the name, and this is
 * where it is read back out.
 */
static enum tile_dir
direction_of(enum cmd command)
{
	switch (command) {
	case cmd_focus_left:
	case cmd_tile_move_left:
	case cmd_tile_resize_left:
		return TILE_LEFT;
	case cmd_focus_up:
	case cmd_tile_move_up:
	case cmd_tile_resize_up:
		return TILE_UP;
	case cmd_focus_down:
	case cmd_tile_move_down:
	case cmd_tile_resize_down:
		return TILE_DOWN;
	default:
		return TILE_RIGHT;
	}
}

static enum tile_layout
layout_of(enum cmd command)
{
	switch (command) {
	case cmd_tile_columns: return TILE_COLUMNS;
	case cmd_tile_rows:    return TILE_ROWS;
	case cmd_tile_grid:    return TILE_GRID;
	case cmd_tile_monocle: return TILE_MONOCLE;
	default:               return TILE_MASTER;
	}
}

/* A window keeps its own geometry unless it is fullscreen or maximized. */
static status
place(struct client *c, struct swc_rectangle g)
{
	if (c->fullscreen || c->maximized)
		return fail("window is fullscreen or maximized");
	/* The layout owns a tiled window's cell: moved here it would sit over
	 * its neighbours until the next arrange put it back. */
	if (c->tiled)
		return fail("window is tiled");
	if (g.width < 1 || g.height < 1)
		return fail("size must be positive");

	chara_place(c, g);
	return ok("");
}

/*
 * A title or app_id as it may appear in a reply. They are the client's to
 * choose, and a reply is one line of tab-separated fields: a newline in a
 * title would end it early and have the rest read as lines of their own,
 * and a tab would shift every field after it.
 */
static const char *
printable(const char *text, const char *missing, char *out, size_t size)
{
	size_t i = 0;

	if (!text)
		return missing;
	for (; text[i] && i + 1 < size; ++i) {
		unsigned char ch = (unsigned char)text[i];
		out[i] = ch < 0x20 || ch == 0x7f ? ' ' : (char)ch;
	}
	out[i] = '\0';
	return out;
}

static struct swc_rectangle
geometry_of(struct client *c)
{
	struct swc_rectangle g;
	/* Successive commands must build on the latest requested placement,
	 * even before a client acknowledges its previous configure. Actual
	 * floating sizes are kept current by the geometry callback. */
	if (!c->tiled && !c->fullscreen && !c->maximized &&
	    !(wm.grab.active && wm.grab.client == c))
		return (struct swc_rectangle){c->x, c->y, c->width, c->height};
	if (!swc_window_get_geometry(c->win, &g))
		g = (struct swc_rectangle){ c->x, c->y, c->width, c->height };
	return g;
}

status
chara_ipc_dispatch(const struct command *cmd, int argc, char **argv)
{
	struct client *c = NULL;
	int32_t a[4] = {0};
	int first = 0;

	if (cmd->selects) {
		const char *selector = argc > 0 ? argv[0] : NULL;
		/* Spelled out or left out, "focused" is the same request. */
		bool named = selector && *selector && strcmp(selector, "focused");

		c = chara_lookup(selector);
		first = argc > 0 ? 1 : 0;
		/* restore with nothing named falls back to the most recently
		 * minimized window, so no focus is not an error there. A name that
		 * resolves to nothing still is. */
		if (!c && (named || cmd->command != cmd_restore))
			return fail("no such window: %s", selector ? selector : "focused");
	}
	if (cmd->command == cmd_restore && c && !c->minimized &&
	    (argc == 0 || !strcmp(argv[0], "focused"))) c = NULL;
	int provided = argc - first;
	if (provided < cmd->argc || provided > cmd->argc + cmd->optional)
		return fail("usage: %s %s", cmd->name, cmd->usage);

	/* Numeric arguments, when the command takes them. The optional ones are
	 * read only as far as they were actually given. */
	{
		int want = cmd->argc + cmd->optional;

		if (want > 4)
			want = 4;
		if (want > provided)
			want = provided;
		for (int i = 0; i < want; ++i) {
			int32_t min = -32768, max = 32767;
			if (cmd->command == cmd_workspace || cmd->command == cmd_move_workspace)
				min = 1, max = CHARA_WORKSPACES;
			else if (cmd->command == cmd_resize_absolute ||
			         (cmd->command == cmd_teleport && i >= 2))
				min = 1, max = 32768;
			if (!number(argv[first + i], min, max, &a[i]))
				return fail("usage: %s %s", cmd->name, cmd->usage);
		}
	}

	/* Mutating another monitor leaves the overview in place. Commands aimed
	 * at its own monitor restore the normal scene before changing it. */
	struct screen *target = c ? c->scr : chara_active_screen();
	if (cmd->command != cmd_overview && cmd->command < cmd_get_geometry &&
	    chara_overview_on_screen(target)) chara_overview_cancel();

	struct swc_rectangle g;
	char label[CHARA_NAME_MAX + 16];
	char text[MAXSIZE / 2], app_id[CHARA_APP_ID_MAX * 4];

	switch (cmd->command) {
	case cmd_move:
		g = geometry_of(c);
		g.x += a[0];
		g.y += a[1];
		return place(c, g);
	case cmd_move_absolute:
		g = geometry_of(c);
		g.x = a[0];
		g.y = a[1];
		return place(c, g);
	case cmd_resize:
		g = geometry_of(c);
		g.width = (int32_t)g.width + a[0] > 0 ? g.width + a[0] : 1;
		g.height = (int32_t)g.height + a[1] > 0 ? g.height + a[1] : 1;
		return place(c, g);
	case cmd_resize_absolute:
		g = geometry_of(c);
		g.width = a[0];
		g.height = a[1];
		return place(c, g);
	case cmd_teleport:
		return place(c, (struct swc_rectangle){ a[0], a[1], a[2], a[3] });
	case cmd_center: {
		struct screen *s = c->scr;
		if (!s || !s->scr)
			return fail("window is not on a monitor");
		struct swc_rectangle area = s->scr->usable_geometry;
		g = geometry_of(c);
		g.x = area.x + ((int32_t)area.width - (int32_t)g.width) / 2;
		g.y = area.y + ((int32_t)area.height - (int32_t)g.height) / 2;
		return place(c, g);
	}
	case cmd_fullscreen:
		chara_set_fullscreen(c, !c->fullscreen, NULL);
		return ok("");
	case cmd_maximize:
		chara_set_maximized(c, !c->maximized);
		return ok("");
	case cmd_pin:
		chara_set_pinned(c, !c->pinned);
		return ok(c->pinned ? "pinned" : "unpinned");
	case cmd_minimize:
		chara_minimize(c);
		return ok("");
	case cmd_restore:
		chara_restore(c);
		return ok("");
	case cmd_hide:
		chara_set_hidden(c, true);
		return ok("");
	case cmd_show:
		chara_set_hidden(c, false);
		chara_bring_up(c);
		return ok("");
	case cmd_raise:
		swc_window_raise(c->win);
		return ok("");
	case cmd_lower:
		swc_window_stack(c->win, 1);
		return ok("");
	case cmd_close:
		swc_window_close(c->win);
		return ok("");
	case cmd_focus:
		chara_bring_up(c);
		chara_focus(c);
		return ok("");
	case cmd_focus_next:
		chara_focus_step(1);
		return ok("");
	case cmd_focus_prev:
		chara_focus_step(-1);
		return ok("");
	case cmd_unfocus:
		chara_focus(NULL);
		return ok("");
	case cmd_workspace:
		chara_ws_go_to(chara_active_screen(), (uint8_t)a[0]);
		return ok("");
	case cmd_move_workspace:
		chara_ws_move_to((uint8_t)a[0], c);
		return ok("");

	/* ------------------------------------------------------------ tiling */
	case cmd_tile:
		if (!chara_tiling_set(c, !c->tiled))
			return fail("the window would not change");
		return ok(c->tiled ? "tiled" : "floating");
	case cmd_tile_workspace: {
		struct screen *s = chara_active_screen();
		uint8_t ws = chara_active_ws();
		bool on;

		if (!s)
			return fail("no monitor");
		on = !chara_tiling_ws_enabled(s, ws);
		chara_tiling_ws_enable(s, ws, on);
		return ok("workspace %u %s", ws, on ? "tiling" : "floating");
	}
	case cmd_tile_promote:
		if (!chara_tiling_promote(c))
			return fail("nothing to promote it over");
		return ok("");
	case cmd_tile_swap:
		if (!chara_tiling_swap(c, wm.cur))
			return fail("both windows must be tiled on the same workspace");
		return ok("");
	case cmd_tile_equalize:
		chara_tiling_equalize(chara_active_screen(), chara_active_ws());
		return ok("");
	case cmd_focus_left:
	case cmd_focus_right:
	case cmd_focus_up:
	case cmd_focus_down:
		if (!chara_focus_dir(direction_of(cmd->command)))
			return fail("nothing that way");
		return ok("");
	case cmd_tile_move_left:
	case cmd_tile_move_right:
	case cmd_tile_move_up:
	case cmd_tile_move_down:
		if (!chara_tiling_move_dir(c, direction_of(cmd->command)))
			return fail("nowhere to move it");
		return ok("");
	case cmd_tile_resize_left:
	case cmd_tile_resize_right:
	case cmd_tile_resize_up:
	case cmd_tile_resize_down: {
		/* The step is optional: left out, the configured one is used, which
		 * is what a key binding wants. */
		int32_t pixels = provided >= 1 ? a[0] : config.tiling.resize_step;

		if (!chara_tiling_resize_dir(c, direction_of(cmd->command), pixels))
			return fail("no fence on that side to move");
		return ok("");
	}
	case cmd_tile_master:
	case cmd_tile_columns:
	case cmd_tile_rows:
	case cmd_tile_grid:
	case cmd_tile_monocle:
		chara_tiling_set_layout(chara_active_screen(), chara_active_ws(),
		                        layout_of(cmd->command));
		return ok("%s", tile_layout_name(layout_of(cmd->command)));
	case cmd_tile_layout_next:
	case cmd_tile_layout_prev: {
		struct screen *s = chara_active_screen();
		struct tile_ws *t;

		chara_tiling_cycle_layout(s, chara_active_ws(),
		    cmd->command == cmd_tile_layout_next ? 1 : -1);
		t = chara_tiling_ws(s, chara_active_ws());
		return t ? ok("%s", tile_layout_name(t->layout)) : fail("no monitor");
	}
	case cmd_tile_master_count:
		if (!chara_tiling_master_count(chara_active_screen(), chara_active_ws(),
		                               a[0]))
			return fail("the master count would not change");
		return ok("");
	case cmd_overview:
		return chara_overview_toggle() ? ok("") : fail("overview unavailable or no matching windows");
	case cmd_zoom:
		/*
		 * swc clamps to a tenth and ten times over; refusing outside that
		 * here means `zoom 5` says so rather than quietly becoming `zoom 10`.
		 */
		if (a[0] < 10 || a[0] > 1000)
			return fail("zoom must be between 10 and 1000 percent");
		swc_set_zoom(a[0] / 100.0f);
		return ok("");
	case cmd_tile_master_ratio:
		if (!chara_tiling_master_ratio(chara_active_screen(), chara_active_ws(),
		                               a[0]))
			return fail("the master ratio would not change");
		return ok("");
	case cmd_get_tiling: {
		struct screen *s = chara_active_screen();
		uint8_t ws = chara_active_ws();
		struct tile_ws *t = chara_tiling_ws(s, ws);

		if (!t)
			return fail("no monitor");
		return ok("%s\t%u\t%s %u %.2f\t%s", tile_layout_name(t->layout),
		          chara_tiling_count(s, ws), tile_side_name(t->master_side),
		          t->master_count, t->master_ratio,
		          chara_tiling_ws_enabled(s, ws) ? "on" : "off");
	}
	case cmd_get_geometry:
		g = geometry_of(c);
		return ok("%d %d %u %u", g.x, g.y, g.width, g.height);
	case cmd_get_pid:
		return ok("%ld", (long)swc_window_get_pid(c->win));
	case cmd_get_title:
		return ok("%s", printable(c->win->title, "", text, sizeof(text)));
	case cmd_get_app_id:
		return ok("%s", printable(c->win->app_id, "", app_id, sizeof(app_id)));
	case cmd_get_id:
		chara_client_label(c, label, sizeof(label));
		return ok("%s", label);
	case cmd_get_focus:
		chara_client_label(wm.cur, label, sizeof(label));
		return ok("%s", label);
	case cmd_get_workspace:
		return ok("%u", chara_active_ws());
	case cmd_get_screen_geometry: {
		struct screen *s = chara_active_screen();
		if (!s)
			return fail("no monitor");
		return ok("%d %d %u %u", s->x, s->y, s->width, s->height);
	}
	case cmd_get_cursor_position: {
		int32_t x, y;
		if (!swc_cursor_position(&x, &y))
			return fail("cursor position unavailable");
		return ok("%d %d", x / 256, y / 256);
	}
	case cmd_list_windows: {
		status s;
		size_t o = 0;

		s.ok = true;
		s.msg[0] = '\0';
		struct client *other;
		wl_list_for_each(other, &wm.clients, link) {
			chara_client_label(other, label, sizeof(label));
			g = geometry_of(other);
			int n = snprintf(s.msg + o, sizeof(s.msg) - o,
			    "%s%s\t%u\t%d %d %u %u\t%s\t%s",
			    o ? "\n" : "", label, other->ws,
			    g.x, g.y, g.width, g.height,
			    printable(other->win->app_id, "-", app_id, sizeof(app_id)),
			    printable(other->win->title, "-", text, sizeof(text)));
			if (n < 0 || (size_t)n >= sizeof(s.msg) - o) {
				s.msg[o] = '\0'; /* whole lines only, not half of one */
				break;
			}
			o += (size_t)n;
		}
		return s;
	}
	case cmd_list_monitors: {
		status s;
		size_t o = 0;

		s.ok = true;
		s.msg[0] = '\0';
		struct screen *other;
		wl_list_for_each(other, &wm.screens, link) {
			int n = snprintf(s.msg + o, sizeof(s.msg) - o,
			    "%s%s\t%d %d %u %u\tworkspace %u",
			    o ? "\n" : "",
			    other->scr ? swc_screen_get_name(other->scr) : "?",
			    other->x, other->y, other->width, other->height, other->ws);
			if (n < 0 || (size_t)n >= sizeof(s.msg) - o) {
				s.msg[o] = '\0'; /* whole lines only, not half of one */
				break;
			}
			o += (size_t)n;
		}
		return s;
	}
	case cmd_reload:
		chara_request_reload();
		return ok("");
	case cmd_quit:
		chara_stop();
		return ok("");
	default:
		return fail("unknown command");
	}
}

/* ------------------------------------------------------------- transport */

#define IPC_MAX_CONNECTIONS 64
#define IPC_TIMEOUT_MS 10000
#define IPC_OUTPUT_LIMIT (16 * MAXSIZE)

struct connection {
	struct wl_list link;
	struct wl_event_source *source, *timeout;
	int fd;
	size_t used, output_used, output_sent;
	bool eof;
	char buffer[MAXSIZE + 1];
	char output[IPC_OUTPUT_LIMIT];
};
static struct wl_list connections = { &connections, &connections };
static unsigned connection_count;

static int listen_fd = -1;
static struct wl_event_source *listen_source;
static struct wl_event_source *resume_timer;
static struct wl_event_loop *event_loop;

/* How long to stop accepting for when the process is out of descriptors. */
#define IPC_BACKOFF_MS 1000

static void
connection_close(struct connection *conn)
{
	wl_event_source_remove(conn->source);
	wl_event_source_remove(conn->timeout);
	wl_list_remove(&conn->link);
	--connection_count;
	close(conn->fd);
	free(conn);
}

/* Split a request line into words; quotes are not interpreted. */
static int
split(char *line, char **argv, int max)
{
	int argc = 0;
	char *save = NULL;

	for (char *token = strtok_r(line, " \t", &save); token && argc < max;
	     token = strtok_r(NULL, " \t", &save))
		argv[argc++] = token;
	return argc;
}

static bool
queue_reply(struct connection *conn, status result)
{
	/* Reclaim bytes sent before appending; never grow per-client storage. */
	if (conn->output_sent) {
		conn->output_used -= conn->output_sent;
		memmove(conn->output, conn->output + conn->output_sent, conn->output_used);
		conn->output_sent = 0;
	}
	size_t room = sizeof(conn->output) - conn->output_used;
	int n = snprintf(conn->output + conn->output_used, room, "%s%s%s\n",
	    result.ok ? "ok" : "error", result.msg[0] ? " " : "", result.msg);
	if (n < 0 || (size_t)n >= room) return false;
	conn->output_used += (size_t)n;
	return true;
}

static bool
handle_line(struct connection *conn, char *line)
{
	char *argv[16];
	int argc = split(line, argv, 16);
	if (!argc) return queue_reply(conn, fail("empty request"));
	const struct command *cmd = NULL;
	for (int i = 0; i < cmd_last; ++i)
		if (commands[i].name && !strcmp(commands[i].name, argv[0])) {
			cmd = &commands[i]; break;
		}
	status result = cmd ? chara_ipc_dispatch(cmd, argc - 1, argv + 1)
	    : fail("unknown command: %s", argv[0]);
	return queue_reply(conn, result);
}

static int
connection_timeout(void *data)
{
	connection_close(data);
	return 0;
}

static int
connection_readable(int fd, uint32_t mask, void *data)
{
	struct connection *conn = data;
	if (mask & WL_EVENT_ERROR) goto close;
	if (!conn->eof && (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP))) {
		ssize_t n = read(fd, conn->buffer + conn->used,
		    sizeof(conn->buffer) - conn->used - 1);
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			goto close;
		if (n == 0) {
			conn->eof = true;
			if (conn->used) {
				if (!queue_reply(conn, fail("request must end with a newline"))) goto close;
				conn->used = 0;
			}
		}
		if (n > 0) {
			conn->used += (size_t)n;
			conn->buffer[conn->used] = '\0';
			/* Embedded NUL must not hide trailing data from the line parser. */
			if (memchr(conn->buffer, '\0', conn->used)) goto close;
			char *line = conn->buffer, *end;
			while ((end = strchr(line, '\n'))) {
				*end = '\0';
				if (!handle_line(conn, line)) goto close;
				line = end + 1;
			}
			conn->used = strlen(line);
			memmove(conn->buffer, line, conn->used + 1);
			if (conn->used + 1 >= sizeof(conn->buffer)) {
				if (!queue_reply(conn, fail("request too long"))) goto close;
				conn->eof = true;
				conn->used = 0;
			}
			wl_event_source_timer_update(conn->timeout, IPC_TIMEOUT_MS);
		}
	}
	while (conn->output_sent < conn->output_used) {
		ssize_t n = send(fd, conn->output + conn->output_sent,
		    conn->output_used - conn->output_sent, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
		if (n <= 0) goto close;
		conn->output_sent += (size_t)n;
		wl_event_source_timer_update(conn->timeout, IPC_TIMEOUT_MS);
	}
	if (conn->output_sent == conn->output_used) {
		conn->output_sent = conn->output_used = 0;
		if (conn->eof) goto close;
	}
	uint32_t events = conn->eof ? 0 : WL_EVENT_READABLE;
	if (conn->output_used) events |= WL_EVENT_WRITABLE;
	wl_event_source_fd_update(conn->source, events);
	return 0;
close:
	connection_close(conn);
	return 0;
}

static int
resume_accepting(void *data)
{
	(void)data;
	if (listen_source)
		wl_event_source_fd_update(listen_source, WL_EVENT_READABLE);
	return 0;
}

/* A pending connection that cannot be accepted keeps the listening socket
 * readable, so returning here would spin the event loop until a descriptor
 * came free. Stop watching it for a moment instead. */
static void
back_off(void)
{
	if (!listen_source || !resume_timer)
		return;
	wl_event_source_fd_update(listen_source, 0);
	wl_event_source_timer_update(resume_timer, IPC_BACKOFF_MS);
}

static int
listener_readable(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;

	for (;;) {
		int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (client < 0) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				break;   /* nothing else waiting */
			case EINTR:
			case ECONNABORTED:
				continue; /* this one went away; try the next */
			default:
				_wrn("ipc: accept: %s", strerror(errno));
				back_off();
				break;
			}
			return 0;
		}
		if (connection_count >= IPC_MAX_CONNECTIONS) {
			close(client);
			back_off();
			return 0;
		}
		struct connection *conn = calloc(1, sizeof(*conn));
		if (!conn) {
			close(client);
			back_off();
			return 0;
		}
		conn->fd = client;
		conn->source = wl_event_loop_add_fd(event_loop, client,
		    WL_EVENT_READABLE, connection_readable, conn);
		if (!conn->source) {
			close(client);
			free(conn);
			back_off();
			return 0;
		}
		conn->timeout = wl_event_loop_add_timer(event_loop, connection_timeout, conn);
		if (!conn->timeout) {
			wl_event_source_remove(conn->source);
			close(client); free(conn); back_off(); return 0;
		}
		wl_list_insert(connections.prev, &conn->link);
		++connection_count;
		wl_event_source_timer_update(conn->timeout, IPC_TIMEOUT_MS);
	}
}

bool
chara_ipc_init(struct wl_event_loop *loop)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char *path = chara_socket_path();

	if (!path)
		return false;
	if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) >=
	    (int)sizeof(address.sun_path)) {
		_wrn("ipc: socket path is too long: %s", path);
		return false;
	}
	unlink(path);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (listen_fd < 0) {
		_wrn("ipc: %s", strerror(errno));
		return false;
	}
	/* bind() applies the umask, so narrow it rather than widening the socket
	 * and fixing the mode afterwards. */
	mode_t saved = umask(0177);
	int bound = bind(listen_fd, (struct sockaddr *)&address, sizeof(address));
	umask(saved);
	if (bound < 0 || listen(listen_fd, 16) < 0) {
		_wrn("ipc: %s: %s", path, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	chmod(path, 0600);
	event_loop = loop;
	listen_source = wl_event_loop_add_fd(loop, listen_fd, WL_EVENT_READABLE,
	                                     listener_readable, NULL);
	if (!listen_source) {
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	resume_timer = wl_event_loop_add_timer(loop, resume_accepting, NULL);
	if (!resume_timer)
		_wrn("ipc: no timer; accepting will not back off under fd pressure");
	_inf("ipc: listening on %s", path);
	return true;
}

void
chara_ipc_finish(void)
{
	struct connection *conn, *tmp;
	wl_list_for_each_safe(conn, tmp, &connections, link)
		connection_close(conn);
	if (listen_source)
		wl_event_source_remove(listen_source);
	if (resume_timer)
		wl_event_source_remove(resume_timer);
	if (listen_fd >= 0) {
		close(listen_fd);
		unlink(chara_socket_path());
	}
	listen_source = NULL;
	resume_timer = NULL;
	listen_fd = -1;
}
