#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>

#include "config.h"

struct wm wm;
struct config config;

static char *config_path;
static bool reload_queued;
static struct wl_event_source *reload_source;
static pid_t bar_pid;

/* ---------------------------------------------------------- socket path */

const char *
chara_socket_path(void)
{
	static char path[108];
	const char *runtime, *display;

	if (path[0])
		return path;
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

static void
load_cursor_theme(void)
{
	static const struct { enum swc_cursor_kind kind; const char *name; } cursors[] = {
		{ SWC_CURSOR_DEFAULT, "left_ptr" },
		{ SWC_CURSOR_BOX, "fleur" },
		{ SWC_CURSOR_CROSS, "crosshair" },
		{ SWC_CURSOR_SIGHT, "hand2" },
		{ SWC_CURSOR_UP, "top_side" },
		{ SWC_CURSOR_DOWN, "bottom_side" },
	};
	int size = config.cursor_size > 0 ? config.cursor_size : 24;

	for (size_t i = 0; i < sizeof(cursors) / sizeof(*cursors); ++i) {
		XcursorImage *image = XcursorLibraryLoadImage(cursors[i].name,
		    config.cursor_theme, size);
		if (!image) {
			swc_clear_cursor_image(cursors[i].kind);
			continue;
		}
		swc_set_cursor_image(cursors[i].kind, image->pixels, image->width,
		                     image->height, image->xhot, image->yhot);
		XcursorImageDestroy(image);
	}
	/* Clients inherit the theme through the environment. */
	if (config.cursor_theme)
		setenv("XCURSOR_THEME", config.cursor_theme, 1);
	char text[16];
	snprintf(text, sizeof(text), "%d", size);
	setenv("XCURSOR_SIZE", text, 1);
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

/* ------------------------------------------------------------------ bar */

static void
update_bar(bool enabled)
{
	if (enabled == (bar_pid > 0))
		return;
	if (!enabled) {
		kill(-bar_pid, SIGTERM);
		bar_pid = 0;
		return;
	}
	char *argv[] = { (char *)"charabar", NULL };
	bar_pid = chara_spawn_process(argv, true);
	if (bar_pid < 0)
		bar_pid = 0;
}

void
chara_child_exited(pid_t pid)
{
	if (pid == bar_pid)
		bar_pid = 0;
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
		if (!s && !wl_list_empty(&wm.screens))
			s = wl_container_of(wm.screens.next, s, link);
		if (s && s != c->scr) {
			struct screen *from = c->scr;
			c->scr = s;
			if (from)
				chara_layout_apply(from, c->ws);
		}
	}
	chara_sync_windows();
	chara_layout_all();
}

static void
on_scr_geometry(void *data)
{
	struct screen *s = data;

	screen_geometry(s);
	chara_layout_apply(s, s->ws);
	reconcile();
}

static void
on_scr_entered(void *data)
{
	struct screen *s = data;

	wm.scr = s;
	if (s->focus)
		chara_focus(s->focus);
}

static void
on_scr_destroy(void *data)
{
	struct screen *s = data;
	struct client *c;

	wl_list_remove(&s->link);
	wl_list_for_each(c, &wm.clients, link)
		if (c->scr == s)
			c->scr = NULL;
	if (wm.scr == s)
		wm.scr = wl_list_empty(&wm.screens) ? NULL
		    : wl_container_of(wm.screens.next, wm.scr, link);
	free(s);
	reconcile();
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
	screen_geometry(s);
	wl_list_insert(wm.screens.prev, &s->link);
	if (!wm.scr)
		wm.scr = s;
	swc_screen_set_handler(scr, &scr_handler, s);
	swc_workspace_set_active(scr, s->ws);
	_inf("monitor %s: %dx%d at %d,%d", name ? name : "?", s->width, s->height,
	     s->x, s->y);
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
	chara_config_finish(&previous);

	load_cursor_theme();
	apply_wallpaper();
	update_bar(config.bar.enabled);
	chara_bind_mouse(config.values.mod);

	struct client *client;
	wl_list_for_each(client, &wm.clients, link)
		chara_decorate(client, wm.cur == client);
	chara_layout_all();
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
	if (bar_pid > 0)
		kill(-bar_pid, SIGTERM);
	chara_startup_finish();
	chara_ipc_finish();
	if (reload_source)
		wl_event_source_remove(reload_source);
}

static void
setup(void)
{
	wl_list_init(&wm.clients);
	wl_list_init(&wm.tiles);
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

	setup();
	if (!chara_config_bindings(&config))
		_wrn("some key bindings could not be installed");
	chara_bind_mouse(config.values.mod);
	load_cursor_theme();
	apply_wallpaper();
	update_bar(config.bar.enabled);
	chara_config_start(&config);

	wl_display_run(wm.dpy);

	cleanup();
	chara_config_finish(&config);
	chara_bindings_finish();
	swc_finalize();
	wl_display_destroy(wm.dpy);
	free(config_path);
	return 0;
}
