#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>

#include "config.h"

struct wm wm;
struct config config;

static char *config_path;
static bool reload_queued;
/* Set once startup has applied the configuration; a monitor that appears
 * after that has to be caught up on it. */
static bool started;
static bool wallpaper_queued;
static struct wl_event_source *reload_source;
static pid_t bar_pid;
static bool bar_restarting;

/* ---------------------------------------------------------- socket path */

const char *
chara_socket_path(void)
{
	static char path[108];
	const char *runtime, *display, *override;

	if (path[0])
		return path;
	/* charactl reads the same variable; listening elsewhere than it looks
	 * would make the override unusable. */
	override = getenv("CHARAWC_SOCKET");
	if (override && *override) {
		snprintf(path, sizeof(path), "%s", override);
		return path;
	}
	runtime = getenv("XDG_RUNTIME_DIR");
	display = getenv("WAYLAND_DISPLAY");
	if (runtime && *runtime == '/')
		snprintf(path, sizeof(path), "%s/charawc-%s.sock", runtime,
		         display ? display : "0");
	else
		snprintf(path, sizeof(path), "/tmp/charawc-%d.sock", (int)getuid());
	return path;
}

/* --------------------------------------------------------------- cursor */

/*
 * Every cursor charaWC can show, and the theme names to try for it. The first
 * name that the theme actually provides wins.
 *
 * Two naming conventions are in use: the CSS names that cursor-shape-v1 is
 * written in ("ns-resize"), which newer themes ship, and the older X names
 * ("sb_v_double_arrow"), which most themes still ship. Listing both means a
 * client asking for a resize cursor gets one from either kind of theme, and
 * falls back to something sensible rather than nothing when a theme is
 * missing a shape entirely.
 */
static const struct {
	enum swc_cursor_kind kind;
	const char *names[4];
} cursors[] = {
	/* charaWC's own mode cursors. */
	{ SWC_CURSOR_DEFAULT,       { "default", "left_ptr", "arrow" } },
	{ SWC_CURSOR_BOX,           { "fleur", "move" } },
	{ SWC_CURSOR_CROSS,         { "crosshair", "cross" } },
	{ SWC_CURSOR_SIGHT,         { "hand2", "pointer" } },
	{ SWC_CURSOR_UP,            { "top_side", "n-resize" } },
	{ SWC_CURSOR_DOWN,          { "bottom_side", "s-resize" } },

	/* cursor-shape-v1, for clients that ask by name. */
	{ SWC_CURSOR_CONTEXT_MENU,  { "context-menu", "left_ptr" } },
	{ SWC_CURSOR_HELP,          { "help", "question_arrow", "whats_this" } },
	{ SWC_CURSOR_POINTER,       { "pointer", "hand2", "hand1" } },
	{ SWC_CURSOR_PROGRESS,      { "progress", "left_ptr_watch", "half-busy" } },
	{ SWC_CURSOR_WAIT,          { "wait", "watch" } },
	{ SWC_CURSOR_CELL,          { "cell", "plus" } },
	{ SWC_CURSOR_CROSSHAIR,     { "crosshair", "cross", "tcross" } },
	{ SWC_CURSOR_TEXT,          { "text", "xterm", "ibeam" } },
	{ SWC_CURSOR_VERTICAL_TEXT, { "vertical-text", "xterm" } },
	{ SWC_CURSOR_ALIAS,         { "alias", "dnd-link", "link" } },
	{ SWC_CURSOR_COPY,          { "copy", "dnd-copy" } },
	{ SWC_CURSOR_MOVE,          { "move", "dnd-move", "fleur" } },
	{ SWC_CURSOR_NO_DROP,       { "no-drop", "dnd-no-drop", "forbidden" } },
	{ SWC_CURSOR_NOT_ALLOWED,   { "not-allowed", "crossed_circle", "forbidden" } },
	{ SWC_CURSOR_GRAB,          { "grab", "openhand", "hand1" } },
	{ SWC_CURSOR_GRABBING,      { "grabbing", "closedhand", "fleur" } },
	{ SWC_CURSOR_E_RESIZE,      { "e-resize", "right_side", "sb_h_double_arrow" } },
	{ SWC_CURSOR_N_RESIZE,      { "n-resize", "top_side", "sb_v_double_arrow" } },
	{ SWC_CURSOR_NE_RESIZE,     { "ne-resize", "top_right_corner" } },
	{ SWC_CURSOR_NW_RESIZE,     { "nw-resize", "top_left_corner" } },
	{ SWC_CURSOR_S_RESIZE,      { "s-resize", "bottom_side", "sb_v_double_arrow" } },
	{ SWC_CURSOR_SE_RESIZE,     { "se-resize", "bottom_right_corner" } },
	{ SWC_CURSOR_SW_RESIZE,     { "sw-resize", "bottom_left_corner" } },
	{ SWC_CURSOR_W_RESIZE,      { "w-resize", "left_side", "sb_h_double_arrow" } },
	{ SWC_CURSOR_EW_RESIZE,     { "ew-resize", "sb_h_double_arrow", "h_double_arrow" } },
	{ SWC_CURSOR_NS_RESIZE,     { "ns-resize", "sb_v_double_arrow", "v_double_arrow" } },
	{ SWC_CURSOR_NESW_RESIZE,   { "nesw-resize", "fd_double_arrow", "size_bdiag" } },
	{ SWC_CURSOR_NWSE_RESIZE,   { "nwse-resize", "bd_double_arrow", "size_fdiag" } },
	{ SWC_CURSOR_COL_RESIZE,    { "col-resize", "sb_h_double_arrow", "split_h" } },
	{ SWC_CURSOR_ROW_RESIZE,    { "row-resize", "sb_v_double_arrow", "split_v" } },
	{ SWC_CURSOR_ALL_SCROLL,    { "all-scroll", "fleur" } },
	{ SWC_CURSOR_ZOOM_IN,       { "zoom-in", "zoom_in" } },
	{ SWC_CURSOR_ZOOM_OUT,      { "zoom-out", "zoom_out" } },
	{ SWC_CURSOR_DND_ASK,       { "dnd-ask", "copy" } },
	{ SWC_CURSOR_ALL_RESIZE,    { "all-resize", "fleur", "move" } },
};

/*
 * Clients pick their own cursors out of the theme named here, so this has to
 * be in the environment before anything is started -- Xwayland above all,
 * which swc forks while it initializes. Setting it afterwards left every X
 * client on the default theme, because a process only reads the environment
 * it was given.
 */
static void
export_cursor_theme(void)
{
	char text[16];

	if (config.cursor_theme) {
		setenv("XCURSOR_THEME", config.cursor_theme, 1);
	}
	snprintf(text, sizeof(text), "%d",
	         config.cursor_size > 0 ? config.cursor_size : 24);
	setenv("XCURSOR_SIZE", text, 1);

	/*
	 * Xcursor looks in ~/.local/share/icons; libxcb-cursor, which the X
	 * window manager uses for the root cursor, does not. A theme installed
	 * there is then found by some clients and not others. Naming the full
	 * list once puts everything on the same footing. Only when the user has
	 * not already said where to look.
	 */
	if (!getenv("XCURSOR_PATH")) {
		const char *home = getenv("HOME");
		char path[PATH_MAX];

		if (home && *home == '/' &&
		    snprintf(path, sizeof(path),
		             "%s/.local/share/icons:%s/.icons:"
		             "/usr/share/icons:/usr/share/pixmaps",
		             home, home) < (int)sizeof(path)) {
			setenv("XCURSOR_PATH", path, 1);
		}
	}
}

static void
load_cursor_theme(void)
{
	static char *loaded_theme;
	static int loaded_size;
	static bool loaded;
	int size = config.cursor_size > 0 ? config.cursor_size : 24;
	const char *theme = config.cursor_theme;

	/* Every shape is read from disk here, up to four names deep. A reload
	 * that did not change the theme has nothing to re-read. */
	if (loaded && size == loaded_size &&
	    (theme == loaded_theme ||
	     (theme && loaded_theme && strcmp(theme, loaded_theme) == 0)))
		return;

	for (size_t i = 0; i < sizeof(cursors) / sizeof(*cursors); ++i) {
		XcursorImage *image = NULL;

		for (size_t n = 0; n < sizeof(cursors[i].names) / sizeof(*cursors[i].names)
		     && cursors[i].names[n] && !image; ++n) {
			image = XcursorLibraryLoadImage(cursors[i].names[n],
			    config.cursor_theme, size);
		}
		if (!image) {
			swc_clear_cursor_image(cursors[i].kind);
			continue;
		}
		swc_set_cursor_image(cursors[i].kind, image->pixels, image->width,
		                     image->height, image->xhot, image->yhot);
		XcursorImageDestroy(image);
	}
	/* Clients inherit the theme through the environment. */
	/* Also here, so a reload that changes the theme reaches anything
	 * started from now on. */
	export_cursor_theme();

	free(loaded_theme);
	loaded_theme = theme ? strdup(theme) : NULL;
	loaded_size = size;
	/* Without the remembered name the next reload has to read it all again,
	 * which is what this ran for; it is still correct. */
	loaded = !theme || loaded_theme;
}

/* ------------------------------------------------------------ wallpaper */

static void
apply_wallpaper(void)
{
	struct swc_prepared_wallpaper *prepared = swc_wallpaper_prepare(
	    config.wallpaper.pixels, config.wallpaper.width, config.wallpaper.height,
	    config.wallpaper.mode, config.wallpaper.background);

	if (prepared)
		swc_wallpaper_commit(prepared);
	else
		_wrn("wallpaper: couldn't prepare the background");
}

/* The wallpaper is scaled per monitor, so a monitor plugged in later has none
 * until it is prepared again. Deferred: new_screen runs before swc lists the
 * screen, and a prepare then would not see it. */
static void
apply_wallpaper_idle(void *data)
{
	(void)data;
	wallpaper_queued = false;
	apply_wallpaper();
}

static void
queue_wallpaper(void)
{
	if (wallpaper_queued || !wm.loop)
		return;
	if (wl_event_loop_add_idle(wm.loop, apply_wallpaper_idle, NULL))
		wallpaper_queued = true;
}

/* ------------------------------------------------------------------ bar */

static void
spawn_bar(void)
{
	/* charabar reads the same file, so it has to be told when the
	 * compositor was started with -c; it defaults to the same path. */
	char *argv[] = { (char *)"charabar", (char *)"-c", config_path, NULL };
	if (!config_path)
		argv[1] = NULL;
	bar_pid = chara_spawn_process(argv, true);
	if (bar_pid < 0)
		bar_pid = 0;
}

/* charabar reads the configuration once, when it starts, so a changed bar
 * section only takes effect if it is restarted. */
static void
update_bar(bool enabled, bool restart)
{
	if (bar_pid > 0 && (!enabled || restart)) {
		/* Respawn once the old bar is gone rather than here: two bars would
		 * claim an exclusive zone each until the first finished exiting.
		 * bar_pid stays set so the exit is recognised as this bar's. */
		bar_restarting = enabled;
		kill(-bar_pid, SIGTERM);
		return;
	}
	if (enabled && bar_pid <= 0)
		spawn_bar();
}

void
chara_child_exited(pid_t pid)
{
	if (pid != bar_pid)
		return;
	bar_pid = 0;
	if (bar_restarting) {
		bar_restarting = false;
		spawn_bar();
	}
}

/* -------------------------------------------------------------- screens */

struct screen *
chara_screen_of(struct swc_screen *scr)
{
	struct screen *s;

	wl_list_for_each(s, &wm.screens, link)
		if (s->scr == scr)
			return s;
	return NULL;
}

static void
screen_geometry(struct screen *s)
{
	s->x = s->scr->geometry.x;
	s->y = s->scr->geometry.y;
	s->width = s->scr->geometry.width;
	s->height = s->scr->geometry.height;
}

/* Windows whose monitor went away, or which now sit over another one. */
static void
reconcile(void)
{
	struct client *c;

	wl_list_for_each(c, &wm.clients, link) {
		struct screen *s = chara_window_screen(c);
		struct screen *from = c->scr;

		if (!s && !wl_list_empty(&wm.screens))
			s = wl_container_of(wm.screens.next, s, link);
		if (s && s != from) {
			uint8_t from_ws = c->ws;

			chara_forget_focus(c, s);
			c->scr = s;
			/* And to the workspace that monitor is showing, as every other
			 * monitor move does; keeping the old number leaves the window
			 * laid out on a workspace that is not on screen. */
			if (c->ws != s->ws) {
				c->ws = s->ws;
				swc_window_set_workspace(c->win, c->ws);
			}
			/* Its place in the old monitor's layout goes with the move;
			 * it needs one in the new monitor's. */
			chara_tiling_reseat(c, from, from_ws);
		}
	}
	chara_sync_windows();
	chara_tiling_dirty_all();
}

static void
on_scr_geometry(void *data)
{
	struct screen *s = data;
	if (chara_overview_on_screen(s)) chara_overview_cancel();

	/* The bar appearing, or a mode change: every workspace on this monitor
	 * has a different area to fill than it did. */
	screen_geometry(s);
	for (uint8_t ws = 1; ws <= CHARA_WORKSPACES; ++ws)
		chara_tiling_dirty(s, ws);
	reconcile();
}

static void
on_scr_entered(void *data)
{
	struct screen *s = data;
	if (chara_overview_on_screen(s)) return;

	wm.scr = s;
	/* Mid-drag the pointer is carrying a window across, so the monitor it
	 * arrives on does not get to take the focus with it. */
	if (wm.grab.active)
		return;
	/* The remembered window may since have been minimized or sent to another
	 * workspace, and minimizing clears it outright, so ask for whatever this
	 * monitor can actually show. An empty monitor leaves the focus alone
	 * rather than taking it away from the one the pointer just left. */
	struct client *next = chara_first_on(s);
	if (next)
		chara_focus(next);
}

/*
 * Carry a point on one monitor to the same place on another, then pull the
 * rectangle back inside it if that one is smaller. Left and top win when it
 * does not fit at all.
 */
static void
translate_into(int32_t *x, int32_t *y, uint32_t width, uint32_t height,
               const struct screen *from, const struct screen *to)
{
	const struct swc_rectangle *area = &to->scr->usable_geometry;
	int64_t nx = (int64_t)*x - from->x + to->x;
	int64_t ny = (int64_t)*y - from->y + to->y;

	if (nx + width > (int64_t)area->x + area->width)
		nx = (int64_t)area->x + area->width - width;
	if (ny + height > (int64_t)area->y + area->height)
		ny = (int64_t)area->y + area->height - height;
	if (nx < area->x)
		nx = area->x;
	if (ny < area->y)
		ny = area->y;
	*x = (int32_t)nx;
	*y = (int32_t)ny;
}

/*
 * A window whose monitor was unplugged. Windows on the workspace that monitor
 * was showing land on the one `to` is showing, so what was in view stays in
 * view; the rest keep their workspace number. Floating windows keep their
 * place relative to the monitor: left at the old coordinates they would sit
 * where no monitor is, with nothing to bring them back.
 */
static void
rehome(struct client *c, struct screen *from, struct screen *to)
{
	uint8_t from_ws = c->ws;

	chara_forget_focus(c, to);
	c->scr = to;
	if (c->ws == from->ws && c->ws != to->ws) {
		c->ws = to->ws;
		swc_window_set_workspace(c->win, c->ws);
	}
	translate_into(&c->x, &c->y, c->width, c->height, from, to);
	translate_into(&c->floating.x, &c->floating.y, c->floating.width,
	               c->floating.height, from, to);
	if (c->fullscreen || c->maximized) {
		chara_update_mode_geometry(c);
	} else if (!c->tiled) {
		struct swc_rectangle g = { c->x, c->y, c->width, c->height };
		swc_window_set_geometry(c->win, &g);
	}
	/* The old monitor is gone, so there is no layout to leave; only one on
	 * this monitor to join. */
	chara_tiling_reseat(c, NULL, from_ws);
	if (wm.cur == c)
		to->focus = c;
}

static void
on_scr_destroy(void *data)
{
	struct screen *s = data, *to;
	const char *name = swc_screen_get_name(s->scr);
	struct monitor_config *m;
	struct client *c;

	/* Shutting down: swc tears every screen down after the compositor, and
	 * there is nowhere to move anything to. */
	if (!wm.running) {
		wl_list_remove(&s->link);
		wl_list_for_each(c, &wm.clients, link)
			if (c->scr == s)
				c->scr = NULL;
		if (wm.scr == s)
			wm.scr = NULL;
		free(s);
		return;
	}
	if (chara_overview_on_screen(s)) chara_overview_cancel();

	/* Plugged back in, it should go back where the configuration puts it. */
	wl_list_for_each(m, &config.monitors, link) {
		if (name && m->matched && !strcmp(m->name, name)) {
			m->matched = false;
			break;
		}
	}

	wl_list_remove(&s->link);
	to = wl_list_empty(&wm.screens) ? NULL
	    : wl_container_of(wm.screens.next, to, link);
	wl_list_for_each(c, &wm.clients, link) {
		if (c->scr != s)
			continue;
		if (to)
			rehome(c, s, to);
		else
			c->scr = NULL;
	}
	if (wm.scr == s)
		wm.scr = to;
	free(s);
	reconcile();
	/* The focused window may have been on a workspace of the old monitor
	 * that is not showing here. */
	if (wm.cur && !wm.cur->visible && wm.scr)
		chara_focus(chara_first_on(wm.scr));
	if (name)
		_inf("monitor %s: removed", name);
}

static const struct swc_screen_handler scr_handler = {
	.destroy = on_scr_destroy,
	.geometry_changed = on_scr_geometry,
	.usable_geometry_changed = on_scr_geometry,
	.entered = on_scr_entered,
};

void
chara_new_screen(struct swc_screen *scr)
{
	struct screen *s = calloc(1, sizeof(*s));
	const char *name = swc_screen_get_name(scr);
	struct monitor_config *m;

	if (!s)
		_err(1, "couldn't allocate a monitor");

	/* Placement from the config, while swc still accepts it. */
	wl_list_for_each(m, &config.monitors, link) {
		if (name && !strcmp(m->name, name) && !m->matched) {
			m->matched = swc_screen_set_initial_position(scr, m->x, m->y);
			break;
		}
	}

	s->scr = scr;
	s->ws = 1;
	chara_tiling_ws_init(s);
	screen_geometry(s);
	wl_list_insert(wm.screens.prev, &s->link);
	if (!wm.scr)
		wm.scr = s;
	swc_screen_set_handler(scr, &scr_handler, s);
	swc_workspace_set_active(scr, s->ws);
	_inf("monitor %s: %dx%d at %d,%d", name ? name : "?", s->width, s->height,
	     s->x, s->y);
	/* Plugged in while running. */
	if (started)
		queue_wallpaper();
}

/* A desktop shell asked for a workspace on one monitor. */
static void
on_workspace_activate(struct swc_screen *scr, uint32_t ws)
{
	struct screen *s = chara_screen_of(scr);

	if (s)
		chara_ws_go_to(s, (uint8_t)ws);
}

static const struct swc_manager manager = {
	.new_screen = chara_new_screen,
	.new_window = chara_new_window,
	.workspace_activate = on_workspace_activate,
};

/* --------------------------------------------------------------- reload */

static int
reload_now(void *data)
{
	(void)data;
	struct config next;
	struct swc_binding_batch *batch;

	reload_queued = false;
	if (!chara_config_init(&next)) {
		_wrn("reload: out of memory");
		return 0;
	}
	if (!chara_config_load(&next, config_path)) {
		_wrn("reload: keeping the running configuration");
		chara_config_finish(&next);
		return 0;
	}
	/* Install bindings atomically: a partial swap would lose keys. */
	batch = swc_binding_batch_create();
	if (!batch || !chara_bindings_prepare(&next, batch)) {
		if (batch)
			swc_binding_batch_discard(batch);
		_wrn("reload: couldn't install key bindings");
		chara_config_finish(&next);
		return 0;
	}
	chara_overview_cancel();
	swc_binding_batch_commit(batch);
	chara_bindings_replace(&next);

	/* exec_once only runs at startup; exec runs on every reload. */
	struct startup_command *c, *tmp;
	wl_list_for_each_safe(c, tmp, &next.exec_once, link) {
		wl_list_remove(&c->link);
		chara_startup_command_free(c);
	}

	/* Not "previous = config; config = next": struct config carries wl_list
	 * heads, which are relocated rather than copied. */
	struct config previous;
	chara_config_move(&previous, &config);
	chara_config_move(&config, &next);
	bool bar_changed = previous.bar.digest != config.bar.digest;
	chara_config_finish(&previous);

	load_cursor_theme();
	apply_wallpaper();
	update_bar(config.bar.enabled, bar_changed);
	chara_bind_mouse(config.values.mod);

	/*
	 * Reloading is an explicit "apply what I have written", so the layouts
	 * go back to what the configuration says -- including a master ratio a
	 * fence drag had moved. Which windows are tiled, and the sizes they
	 * were given within a layout, are the session's and are kept.
	 */
	struct screen *screen;
	wl_list_for_each(screen, &wm.screens, link)
		chara_tiling_ws_reset(screen);
	chara_tiling_dirty_all();

	struct client *client;
	wl_list_for_each(client, &wm.clients, link)
		chara_decorate(client, wm.cur == client);
	chara_config_start(&config);
	_inf("reload: configuration applied");
	return 0;
}

void
chara_request_reload(void)
{
	if (reload_queued || !reload_source)
		return;
	reload_queued = true;
	wl_event_source_timer_update(reload_source, 1);
}

/* ----------------------------------------------------------------- life */

void
chara_stop(void)
{
	wm.running = false;
	if (wm.dpy)
		wl_display_terminate(wm.dpy);
}

static int
on_signal(int number, void *data)
{
	(void)number, (void)data;
	chara_stop();
	return 0;
}

static void
cleanup(void)
{
	chara_overview_cancel();
	if (bar_pid > 0)
		kill(-bar_pid, SIGTERM);
	chara_startup_finish();
	chara_tiling_finish();
	chara_ipc_finish();
	if (reload_source)
		wl_event_source_remove(reload_source);
}

static void
setup(void)
{
	wl_list_init(&wm.clients);
	wl_list_init(&wm.screens);
	wm.running = true;

	wm.dpy = wl_display_create();
	if (!wm.dpy)
		_err(1, "couldn't create a Wayland display");
	wm.loop = wl_display_get_event_loop(wm.dpy);

	if (!swc_initialize(wm.dpy, wm.loop, &manager))
		_err(1, "couldn't initialize neuswc");

	const char *socket = wl_display_add_socket_auto(wm.dpy);
	if (!socket)
		_err(1, "couldn't add a Wayland socket");
	setenv("WAYLAND_DISPLAY", socket, 1);
	_inf("WAYLAND_DISPLAY=%s", socket);

	/* Degraded, not broken: key bindings dispatch without the socket. */
	if (!chara_ipc_init(wm.loop))
		_wrn("continuing without a control socket: charactl cannot reach "
		     "this session");
	if (!chara_startup_init(wm.loop))
		_err(1, "couldn't set up startup commands");
	chara_tiling_init(wm.loop);

	reload_source = wl_event_loop_add_timer(wm.loop, reload_now, NULL);
	wl_event_loop_add_signal(wm.loop, SIGINT, on_signal, NULL);
	wl_event_loop_add_signal(wm.loop, SIGTERM, on_signal, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void
usage(FILE *out, const char *name)
{
	fprintf(out, "usage: %s [-c config.lua] [-C] [-W] [-h]\n"
	             "  -c  configuration file (default ~/.config/charawc/config.lua)\n"
	             "  -C  check the configuration and exit\n"
	             "  -W, --write-config  write the starter configuration if there\n"
	             "      is none, then exit; an existing file is left alone\n", name);
}

int
main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "write-config", no_argument, NULL, 'W' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	bool check_only = false, write_only = false;
	int option;

	while ((option = getopt_long(argc, argv, "c:ChW", long_options, NULL)) != -1) {
		switch (option) {
		case 'c':
			config_path = strdup(optarg);
			break;
		case 'C':
			check_only = true;
			break;
		case 'W':
			write_only = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}
	if (!config_path)
		config_path = chara_config_path("config.lua");

	if (write_only) {
		if (!config_path)
			_err(1, "couldn't work out where the configuration lives; "
			        "set HOME or XDG_CONFIG_HOME");
		if (chara_config_write_example(config_path)) {
			printf("wrote a starter configuration to %s\n", config_path);
			return 0;
		}
		if (errno == EEXIST) {
			printf("%s already exists; left alone\n", config_path);
			return 0;
		}
		fprintf(stderr, "charawc: couldn't write %s: %s\n", config_path,
		        strerror(errno));
		return 1;
	}

	if (!chara_config_init(&config))
		_err(1, "out of memory");

	/* A first run should leave a file behind to edit, not invisible defaults.
	 * Checking a configuration must not create one, though: absence is not a
	 * failure, so -C reports it and succeeds rather than holding up a login. */
	if (!config_path) {
		_wrn("couldn't work out where the configuration lives; "
		     "using built-in defaults");
	} else if (access(config_path, F_OK) < 0 && errno == ENOENT) {
		if (check_only) {
			printf("%s: no configuration yet; built-in defaults would be "
			       "used\n", config_path);
			return 0;
		}
		if (chara_config_write_example(config_path))
			_inf("wrote a starter configuration to %s", config_path);
		else
			_wrn("couldn't write %s: %s", config_path, strerror(errno));
	}

	if (!chara_config_load(&config, config_path)) {
		if (check_only)
			return 1;
		/* The parser fills the configuration as it reads it, so a file that
		 * fails halfway through leaves a partly applied one behind - with no
		 * bindings at all if it failed before reaching them, and so no way
		 * to reload or log out. Start again from the built-in defaults. */
		_wrn("%s: not applied; continuing with built-in defaults",
		     config_path ? config_path : "configuration");
		chara_config_finish(&config);
		if (!chara_config_init(&config) || !chara_config_load(&config, NULL))
			_err(1, "out of memory");
	}
	if (check_only) {
		printf("%s: ok\n", config_path ? config_path : "defaults");
		return 0;
	}

	/* Before setup(): swc_initialize forks Xwayland, and it can only
	 * inherit what is already in the environment. */
	export_cursor_theme();
	setup();
	if (!chara_config_bindings(&config))
		_wrn("some key bindings could not be installed");
	chara_bind_mouse(config.values.mod);
	load_cursor_theme();
	apply_wallpaper();
	started = true;
	update_bar(config.bar.enabled, false);
	chara_config_start(&config);

	wl_display_run(wm.dpy);
	/* swc can end the loop itself (Ctrl+Alt+Backspace), without chara_stop. */
	wm.running = false;

	cleanup();
	chara_config_finish(&config);
	chara_bindings_finish();
	swc_finalize();
	wl_display_destroy(wm.dpy);
	free(config_path);
	return 0;
}
