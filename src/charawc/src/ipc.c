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
	[cmd_floating]         = { "floating", cmd_floating, 0, true, "<window>" },
	[cmd_hide]             = { "hide", cmd_hide, 0, true, "<window>" },
	[cmd_show]             = { "show", cmd_show, 0, true, "<window>" },
	[cmd_raise]            = { "raise", cmd_raise, 0, true, "<window>" },
	[cmd_lower]            = { "lower", cmd_lower, 0, true, "<window>" },
	[cmd_close]            = { "close", cmd_close, 0, true, "<window>" },
	[cmd_focus]            = { "focus", cmd_focus, 0, true, "<window>" },
	[cmd_focus_next]       = { "focus_next", cmd_focus_next, 0, false, "" },
	[cmd_focus_prev]       = { "focus_prev", cmd_focus_prev, 0, false, "" },
	[cmd_unfocus]          = { "unfocus", cmd_unfocus, 0, false, "" },
	[cmd_workspace]        = { "workspace", cmd_workspace, 1, false, "<1-9>" },
	[cmd_move_workspace]   = { "move_workspace", cmd_move_workspace, 1, true, "<window> <1-9>" },
	[cmd_layout]           = { "layout", cmd_layout, 1, false, "floating|split|quad" },
	[cmd_layout_axis]      = { "layout_axis", cmd_layout_axis, 1, false, "vertical|horizontal" },
	[cmd_layout_max]       = { "layout_max", cmd_layout_max, 1, false, "<1-16>" },
	[cmd_get_geometry]     = { "get_geometry", cmd_get_geometry, 0, true, "<window>" },
	[cmd_get_pid]          = { "get_pid", cmd_get_pid, 0, true, "<window>" },
	[cmd_get_title]        = { "get_title", cmd_get_title, 0, true, "<window>" },
	[cmd_get_app_id]       = { "get_app_id", cmd_get_app_id, 0, true, "<window>" },
	[cmd_get_id]           = { "get_id", cmd_get_id, 0, true, "<window>" },
	[cmd_get_focus]        = { "get_focus", cmd_get_focus, 0, false, "" },
	[cmd_get_workspace]    = { "get_workspace", cmd_get_workspace, 0, false, "" },
	[cmd_get_layout]       = { "get_layout", cmd_get_layout, 0, false, "" },
	[cmd_get_screen_geometry] = { "get_screen_geometry", cmd_get_screen_geometry, 0, false, "" },
	[cmd_get_cursor_position] = { "get_cursor_position", cmd_get_cursor_position, 0, false, "" },
	[cmd_list_windows]     = { "list_windows", cmd_list_windows, 0, false, "" },
	[cmd_list_monitors]    = { "list_monitors", cmd_list_monitors, 0, false, "" },
	[cmd_reload]           = { "reload", cmd_reload, 0, false, "" },
	[cmd_quit]             = { "quit", cmd_quit, 0, false, "" },
};

static status
ok(const char *fmt, ...)
{
	status s = { .ok = true };
	va_list list;
	va_start(list, fmt);
	vsnprintf(s.msg, sizeof(s.msg), fmt, list);
	va_end(list);
	return s;
}

static status
fail(const char *fmt, ...)
{
	status s = { .ok = false };
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

/* Floating windows keep their own geometry; tiled ones are laid out. */
static status
place(struct client *c, struct swc_rectangle g)
{
	if (c->tiled)
		return fail("window is tiled; use 'floating' first");
	if (c->fullscreen || c->maximized)
		return fail("window is fullscreen or maximized");
	if (g.width < 1 || g.height < 1)
		return fail("size must be positive");

	swc_window_set_geometry(c->win, &g);
	c->x = g.x;
	c->y = g.y;
	c->width = g.width;
	c->height = g.height;
	return ok("");
}

static struct swc_rectangle
geometry_of(struct client *c)
{
	struct swc_rectangle g;
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
		c = chara_lookup(selector);
		first = argc > 0 ? 1 : 0;
		if (!c && cmd->command != cmd_restore)
			return fail("no such window: %s", selector ? selector : "focused");
	}
	if (argc - first < cmd->argc)
		return fail("usage: %s %s", cmd->name, cmd->usage);

	/* Numeric arguments, when the command takes them. */
	if (cmd->command != cmd_layout && cmd->command != cmd_layout_axis) {
		for (int i = 0; i < cmd->argc && i < 4; ++i) {
			int32_t min = -32768, max = 32767;
			if (cmd->command == cmd_workspace || cmd->command == cmd_move_workspace)
				min = 1, max = CHARA_WORKSPACES;
			else if (cmd->command == cmd_layout_max)
				min = 1, max = 16;
			else if (cmd->command == cmd_resize_absolute ||
			         (cmd->command == cmd_teleport && i >= 2))
				min = 1, max = 32768;
			if (!number(argv[first + i], min, max, &a[i]))
				return fail("usage: %s %s", cmd->name, cmd->usage);
		}
	}

	struct swc_rectangle g;
	char label[CHARA_NAME_MAX + 16];

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
	case cmd_minimize:
		chara_minimize(c);
		return ok("");
	case cmd_restore:
		chara_restore(c);
		return ok("");
	case cmd_floating:
		chara_layout_set_floating(c, c->tiled);
		return ok("%s", c->tiled ? "tiled" : "floating");
	case cmd_hide:
		swc_window_hide(c->win);
		c->visible = false;
		return ok("");
	case cmd_show:
		swc_window_show(c->win);
		c->visible = true;
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
	case cmd_layout: {
		const char *mode = argv[first];
		if (!strcmp(mode, "floating")) config.layout.mode = LAYOUT_FLOATING;
		else if (!strcmp(mode, "split")) config.layout.mode = LAYOUT_SPLIT;
		else if (!strcmp(mode, "quad")) config.layout.mode = LAYOUT_QUAD;
		else return fail("usage: layout %s", cmd->usage);
		chara_layout_all();
		return ok("");
	}
	case cmd_layout_axis: {
		const char *axis = argv[first];
		if (!strcmp(axis, "vertical")) config.layout.axis = SPLIT_VERTICAL;
		else if (!strcmp(axis, "horizontal")) config.layout.axis = SPLIT_HORIZONTAL;
		else return fail("usage: layout_axis %s", cmd->usage);
		chara_layout_all();
		return ok("");
	}
	case cmd_layout_max:
		config.layout.max = (unsigned)a[0];
		chara_layout_all();
		return ok("");
	case cmd_get_geometry:
		g = geometry_of(c);
		return ok("%d %d %u %u", g.x, g.y, g.width, g.height);
	case cmd_get_pid:
		return ok("%ld", (long)swc_window_get_pid(c->win));
	case cmd_get_title:
		return ok("%s", c->win->title ? c->win->title : "");
	case cmd_get_app_id:
		return ok("%s", c->win->app_id ? c->win->app_id : "");
	case cmd_get_id:
		chara_client_label(c, label, sizeof(label));
		return ok("%s", label);
	case cmd_get_focus:
		chara_client_label(wm.cur, label, sizeof(label));
		return ok("%s", label);
	case cmd_get_workspace:
		return ok("%u", chara_active_ws());
	case cmd_get_layout: {
		static const char *const modes[] = { "floating", "split", "quad" };
		return ok("%s %s %u", modes[config.layout.mode],
		          config.layout.axis == SPLIT_VERTICAL ? "vertical" : "horizontal",
		          config.layout.max);
	}
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
		status s = { .ok = true };
		size_t o = 0;
		struct client *other;
		wl_list_for_each(other, &wm.clients, link) {
			chara_client_label(other, label, sizeof(label));
			g = geometry_of(other);
			int n = snprintf(s.msg + o, sizeof(s.msg) - o,
			    "%s%s\t%u\t%s\t%d %d %u %u\t%s\t%s",
			    o ? "\n" : "", label, other->ws,
			    other->tiled ? "tiled" : "floating",
			    g.x, g.y, g.width, g.height,
			    other->win->app_id ? other->win->app_id : "-",
			    other->win->title ? other->win->title : "-");
			if (n < 0 || (size_t)n >= sizeof(s.msg) - o)
				break;
			o += (size_t)n;
		}
		return s;
	}
	case cmd_list_monitors: {
		status s = { .ok = true };
		size_t o = 0;
		struct screen *other;
		wl_list_for_each(other, &wm.screens, link) {
			int n = snprintf(s.msg + o, sizeof(s.msg) - o,
			    "%s%s\t%d %d %u %u\tworkspace %u",
			    o ? "\n" : "",
			    other->scr ? swc_screen_get_name(other->scr) : "?",
			    other->x, other->y, other->width, other->height, other->ws);
			if (n < 0 || (size_t)n >= sizeof(s.msg) - o)
				break;
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

struct connection {
	struct wl_event_source *source;
	int fd;
	size_t used;
	char buffer[MAXSIZE];
};

static int listen_fd = -1;
static struct wl_event_source *listen_source;
static struct wl_event_loop *event_loop;

static void
connection_close(struct connection *conn)
{
	wl_event_source_remove(conn->source);
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

static void
handle_line(struct connection *conn, char *line)
{
	char *argv[16];
	int argc = split(line, argv, 16);
	status result;

	if (!argc)
		return;

	const struct command *cmd = NULL;
	for (int i = 0; i < cmd_last; ++i)
		if (commands[i].name && !strcmp(commands[i].name, argv[0]))
			cmd = &commands[i];

	if (!cmd)
		result = fail("unknown command: %s", argv[0]);
	else
		result = chara_ipc_dispatch(cmd, argc - 1, argv + 1);

	dprintf(conn->fd, "%s%s%s\n", result.ok ? "ok" : "error",
	        result.msg[0] ? " " : "", result.msg);
}

static int
connection_readable(int fd, uint32_t mask, void *data)
{
	struct connection *conn = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		connection_close(conn);
		return 0;
	}
	ssize_t n = read(fd, conn->buffer + conn->used,
	                 sizeof(conn->buffer) - conn->used - 1);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		connection_close(conn);
		return 0;
	}
	conn->used += (size_t)n;
	conn->buffer[conn->used] = '\0';

	char *line = conn->buffer, *end;
	while ((end = strchr(line, '\n'))) {
		*end = '\0';
		handle_line(conn, line);
		line = end + 1;
	}
	conn->used = strlen(line);
	memmove(conn->buffer, line, conn->used + 1);
	if (conn->used + 1 >= sizeof(conn->buffer)) {
		dprintf(conn->fd, "error request too long\n");
		connection_close(conn);
	}
	return 0;
}

static int
listener_readable(int fd, uint32_t mask, void *data)
{
	(void)mask, (void)data;

	for (;;) {
		int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (client < 0)
			return 0;
		struct connection *conn = calloc(1, sizeof(*conn));
		if (!conn) {
			close(client);
			return 0;
		}
		conn->fd = client;
		conn->source = wl_event_loop_add_fd(event_loop, client,
		    WL_EVENT_READABLE, connection_readable, conn);
		if (!conn->source) {
			close(client);
			free(conn);
		}
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
	_inf("ipc: listening on %s", path);
	return true;
}

void
chara_ipc_finish(void)
{
	if (listen_source)
		wl_event_source_remove(listen_source);
	if (listen_fd >= 0) {
		close(listen_fd);
		unlink(chara_socket_path());
	}
	listen_source = NULL;
	listen_fd = -1;
}
