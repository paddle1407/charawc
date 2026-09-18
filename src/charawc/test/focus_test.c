/*
 * Exercises the real window.c and border.c against stubbed compositor calls.
 *
 * Three regressions live here:
 *   - screen->focus is remembered per monitor, so a window focused on more
 *     than one is pointed at from more than one place. Destroying it, or
 *     moving it, has to clear every one of them.
 *   - a client asking to go fullscreen names a monitor, and the one it names
 *     is usually just the first output rather than one the user picked.
 *   - decorations are painted outside the content they frame, so a fullscreen
 *     window drawn with a border paints it onto the monitor next door.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

struct wm wm;
struct config config;

/* Stubs: the window manager talks to swc and to the control socket. */
static struct swc_screen *fullscreen_on;
static uint32_t border_inner_width, border_outer_width;
static bool decor_cleared, decor_prepared;
static struct swc_decor last_decor;

void swc_window_set_border(struct swc_window *w, uint32_t ic, uint32_t iw,
                           uint32_t oc, uint32_t ow)
{ border_inner_width = iw; border_outer_width = ow; }
void swc_window_set_decor(struct swc_window *w, const struct swc_decor *d)
{ decor_cleared = d == NULL; }
struct swc_prepared_decor *swc_decor_prepare(const struct swc_decor *d, uint32_t width)
{ decor_prepared = true; last_decor = *d; return NULL; }
void swc_decor_discard(struct swc_prepared_decor *p) {}
void swc_window_apply_decor(struct swc_window *w, struct swc_prepared_decor *p) {}

void swc_window_set_fullscreen(struct swc_window *w, struct swc_screen *s) { fullscreen_on = s; }

/* The tiling writes geometry rather than reading it back, so record what each
 * window was given and hand the same thing back when it is asked for. */
static struct swc_rectangle geometry[4];
static bool geometry_set[4];
static unsigned window_index(const struct swc_window *w);

void swc_window_set_geometry(struct swc_window *w, const struct swc_rectangle *g)
{ unsigned i = window_index(w); if (i < 4) { geometry[i] = *g; geometry_set[i] = true; } }
void swc_window_set_tiled_edges(struct swc_window *w, uint32_t edges) {}
void swc_overlay_set_box(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         uint32_t color, uint32_t width) {}
void swc_overlay_clear(void) {}
void swc_set_cursor(enum swc_cursor_kind kind) {}
bool swc_pointer_grab_begin(swc_pointer_motion_handler h, void *d) { return false; }
void swc_pointer_grab_end(void) {}
void swc_window_set_tiled(struct swc_window *w) {}
void swc_window_set_stacked(struct swc_window *w) {}
void swc_window_set_pinned(struct swc_window *w, bool p) {}
void swc_window_set_minimized(struct swc_window *w, bool m) {}
void swc_window_set_workspace(struct swc_window *w, uint32_t ws) {}
void swc_window_show(struct swc_window *w) {}
void swc_window_show_in_place(struct swc_window *w) {}
void swc_window_hide(struct swc_window *w) {}
void swc_window_raise(struct swc_window *w) {}
void swc_window_focus(struct swc_window *w) {}
void swc_window_close(struct swc_window *w) {}
void swc_window_begin_move(struct swc_window *w) {}
void swc_window_end_move(struct swc_window *w) {}
void swc_window_begin_resize(struct swc_window *w, uint32_t e) {}
void swc_window_end_resize(struct swc_window *w) {}
void swc_window_set_handler(struct swc_window *w, const struct swc_window_handler *h, void *d) {}
bool swc_window_get_geometry(const struct swc_window *w, struct swc_rectangle *g)
{
	unsigned i = window_index(w);
	if (i >= 4 || !geometry_set[i])
		return false;
	*g = geometry[i];
	return true;
}
struct swc_window *swc_window_at(int32_t x, int32_t y) { return NULL; }
bool swc_cursor_position(int32_t *x, int32_t *y) { return false; }
int swc_add_binding(enum swc_binding_type t, uint32_t m, uint32_t v,
                    swc_binding_handler h, void *d) { return 0; }

status chara_ipc_dispatch(const struct command *cmd, int argc, char **argv)
{ return (status){ .ok = true }; }
const struct command commands[cmd_last];

/* Two monitors side by side, as in the crash report. */
static struct swc_screen swc_screens[2];
static struct screen screens[2];
static struct swc_window windows[4];
static struct client clients[4];

static unsigned
window_index(const struct swc_window *w)
{
	for (unsigned i = 0; i < 4; ++i)
		if (&windows[i] == w)
			return i;
	return 4;
}

struct screen *
chara_screen_of(struct swc_screen *scr)
{
	for (unsigned i = 0; i < 2; ++i)
		if (screens[i].scr == scr)
			return &screens[i];
	return NULL;
}

static int failures;

static void
check(const char *what, bool ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		++failures;
}

static void
setup(void)
{
	wl_list_init(&wm.clients);
	wl_list_init(&wm.screens);
	wm.cur = NULL;
	wm.grab = (struct grab){0};
	memset(screens, 0, sizeof(screens));
	memset(clients, 0, sizeof(clients));
	border_inner_width = border_outer_width = 0;
	decor_cleared = decor_prepared = false;

	memset(geometry, 0, sizeof(geometry));
	memset(geometry_set, 0, sizeof(geometry_set));

	/* Tiling with no gaps, so the numbers below are the frame and nothing
	 * else. A 2px ring and a 24px titlebar: 2 to the sides and the bottom,
	 * 26 off the top. */
	config.tiling = (struct tiling_config){
		.enabled = true,
		.layout = TILE_MASTER,
		.master_side = TILE_SIDE_LEFT,
		.master_ratio = 0.5,
		.master_count = 1,
		.resize_step = 40,
		.insert = TILE_INSERT_END,
	};

	if (!config.decoration)
		config.decoration = decor_create();
	config.decoration->titlebar.enabled = true;
	config.values.ring_count = 1;
	config.values.rings[0] = (struct ring){ .width = 2, .focused = 0xffffffff };
	free(config.values.title_format);
	config.values.title_format = strdup("%t");

	for (unsigned i = 0; i < 2; ++i) {
		chara_tiling_ws_reset(&screens[i]);
		swc_screens[i].geometry = (struct swc_rectangle){ (int32_t)i * 1920, 0, 1920, 1080 };
		swc_screens[i].usable_geometry = swc_screens[i].geometry;
		screens[i].scr = &swc_screens[i];
		screens[i].x = (int32_t)i * 1920;
		screens[i].y = 0;
		screens[i].width = 1920;
		screens[i].height = 1080;
		screens[i].ws = 1;
		wl_list_insert(wm.screens.prev, &screens[i].link);
	}
	wm.scr = &screens[0];
}

static struct client *
add_client(unsigned n, struct screen *on)
{
	struct client *c = &clients[n];

	c->win = &windows[n];
	c->scr = on;
	c->ws = on->ws;
	c->id = n + 1;
	c->visible = true;
	c->movable = c->resizable = c->titlebar = true;
	wl_list_insert(wm.clients.prev, &c->link);
	return c;
}

/* A window focused on one monitor and then moved to the other used to leave
 * the first monitor pointing at it. */
static void
test_moving_clears_the_old_monitor(void)
{
	struct client *c;

	setup();
	c = add_client(0, &screens[1]);
	chara_focus(c);
	check("focusing records the window on its own monitor",
	      screens[1].focus == c);

	chara_forget_focus(c, &screens[0]);
	c->scr = &screens[0];
	chara_focus(c);
	check("moving to the other monitor clears the first",
	      screens[1].focus == NULL && screens[0].focus == c);
}

/* The crash: the game opened on one monitor, went fullscreen on the other,
 * and quit. Whatever cleans up after it has to clear both monitors, or the
 * next pointer crossing focuses freed memory. */
static void
test_closing_clears_every_monitor(void)
{
	struct client *c;

	setup();
	c = add_client(0, &screens[1]);
	chara_focus(c);
	config.values.fullscreen_follows_client = true;
	chara_set_fullscreen(c, true, &swc_screens[0]);
	chara_focus(c);
	check("fullscreen on another monitor moves the window there",
	      c->scr == &screens[0] && screens[0].focus == c);
	check("and clears the monitor it came from", screens[1].focus == NULL);

	chara_forget_focus(c, NULL);
	check("forgetting it outright leaves no monitor pointing at it",
	      screens[0].focus == NULL && screens[1].focus == NULL);
}

/* A client naming a monitor is a hint, and by default not a good one. */
static void
test_fullscreen_monitor_choice(void)
{
	struct client *c;

	setup();
	c = add_client(0, &screens[1]);
	chara_focus(c);
	config.values.fullscreen_follows_client = false;
	fullscreen_on = NULL;
	chara_set_fullscreen(c, true, &swc_screens[0]);
	check("by default fullscreen stays on the window's own monitor",
	      c->scr == &screens[1] && fullscreen_on == &swc_screens[1]);

	setup();
	c = add_client(0, &screens[1]);
	chara_focus(c);
	config.values.fullscreen_follows_client = true;
	fullscreen_on = NULL;
	chara_set_fullscreen(c, true, &swc_screens[0]);
	check("fullscreen_follows_client honors the monitor asked for",
	      c->scr == &screens[0] && fullscreen_on == &swc_screens[0]);

	/* A monitor charaWC does not know about is no reason to go nowhere. */
	setup();
	c = add_client(0, &screens[1]);
	chara_focus(c);
	config.values.fullscreen_follows_client = true;
	fullscreen_on = NULL;
	chara_set_fullscreen(c, true, &(struct swc_screen){0});
	check("an unknown monitor falls back to the window's own",
	      c->scr == &screens[1] && fullscreen_on == &swc_screens[1]);
}

/*
 * The frame is drawn outside the content, so on a window that already fills
 * the monitor it can only land on the next one along -- and take the window
 * into that monitor's outputs with it, since a view's screens come from what
 * it paints.
 */
static void
test_fullscreen_is_undecorated(void)
{
	struct client *c;

	setup();
	c = add_client(0, &screens[0]);
	chara_decorate(c, true);
	check("a normal window gets a border",
	      border_inner_width == 2);
	check("and a titlebar", decor_prepared && last_decor.titlebar.enabled);

	border_inner_width = 99;
	decor_cleared = decor_prepared = false;
	c->fullscreen = true;
	chara_decorate(c, true);
	check("a fullscreen window gets no border at all",
	      border_inner_width == 0 && border_outer_width == 0);
	check("and no titlebar or edge decoration",
	      decor_cleared && !decor_prepared);
	check("its titlebar takes up no height either",
	      chara_titlebar_height(c) == 0);

	/* Leaving fullscreen puts the frame back. */
	c->fullscreen = false;
	chara_decorate(c, true);
	check("leaving fullscreen restores the frame",
	      border_inner_width == 2 && decor_prepared);
}

/* ------------------------------------------------------------- tiling */

static bool
is_rect(unsigned window, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	return geometry_set[window] && geometry[window].x == x &&
	       geometry[window].y == y && geometry[window].width == width &&
	       geometry[window].height == height;
}

/*
 * The glue, rather than the geometry: tiling_test already checks that the
 * layout engine divides a rectangle correctly. What is worth checking here is
 * that charaWC's windows reach it and come back with the frame allowed for.
 */
static void
test_tiling_places_windows(void)
{
	struct client *a, *b;

	setup();
	a = add_client(0, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_flush();
	check("one tiled window fills the workspace inside its frame",
	      is_rect(0, 2, 26, 1916, 1052));

	b = add_client(1, &screens[0]);
	chara_tiling_set(b, true);
	chara_tiling_flush();
	check("a second splits it, master first",
	      is_rect(0, 2, 26, 956, 1052) && is_rect(1, 962, 26, 956, 1052));
	check("and they are ordered as they arrived",
	      a->tile_order == 0 && b->tile_order == 1);
}

static void
test_tiling_survives_fullscreen(void)
{
	struct client *a, *b;

	setup();
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_set(b, true);
	chara_tiling_flush();

	chara_set_fullscreen(a, true, NULL);
	chara_tiling_flush();
	check("a window going fullscreen leaves the grid to the others",
	      is_rect(1, 2, 26, 1916, 1052));
	check("but keeps its membership and its place",
	      a->tiled && a->tile_order == 0);

	chara_set_fullscreen(a, false, NULL);
	chara_tiling_flush();
	check("and comes back to the cell it had",
	      is_rect(0, 2, 26, 956, 1052) && is_rect(1, 962, 26, 956, 1052));
}

static void
test_tiling_swaps_and_resizes(void)
{
	struct client *a, *b;

	setup();
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_set(b, true);
	chara_tiling_flush();

	check("moving right trades places with the neighbour",
	      chara_tiling_move_dir(a, TILE_RIGHT));
	chara_tiling_flush();
	check("so the windows change cells and the cells stay put",
	      is_rect(1, 2, 26, 956, 1052) && is_rect(0, 962, 26, 956, 1052) &&
	      b->tile_order == 0 && a->tile_order == 1);

	check("moving off the right of the monitor carries on to the next one",
	      chara_tiling_move_dir(a, TILE_RIGHT) && a->scr == &screens[1]);
	chara_tiling_flush();
	check("where it fills the workspace on its own",
	      is_rect(0, 1922, 26, 1916, 1052));
	a->scr = &screens[0];
	chara_tiling_reseat(a, &screens[1], a->ws);
	chara_tiling_flush();

	check("growing the master's right edge moves the fence",
	      chara_tiling_resize_dir(b, TILE_RIGHT, 100));
	chara_tiling_flush();
	check("by the pixels asked for, out of the other window",
	      is_rect(1, 2, 26, 1056, 1052) && is_rect(0, 1062, 26, 856, 1052));
}

static void
test_untiling_restores_the_window(void)
{
	struct client *a, *b;

	setup();
	a = add_client(0, &screens[0]);
	a->x = 300;
	a->y = 200;
	a->width = 640;
	a->height = 480;
	chara_tiling_set(a, true);
	b = add_client(1, &screens[0]);
	chara_tiling_set(b, true);
	chara_tiling_flush();
	check("tiling remembers where a window was floating",
	      a->floating.x == 300 && a->floating.width == 640);

	chara_tiling_set(a, false);
	chara_tiling_flush();
	check("and puts it back there when it leaves",
	      is_rect(0, 300, 200, 640, 480) && !a->tiled);
	check("while the one still tiled takes the whole workspace",
	      is_rect(1, 2, 26, 1916, 1052));
	check("and the orders close up behind it", b->tile_order == 0);
}

int
main(void)
{
	test_moving_clears_the_old_monitor();
	test_closing_clears_every_monitor();
	test_fullscreen_monitor_choice();
	test_fullscreen_is_undecorated();
	test_tiling_places_windows();
	test_tiling_survives_fullscreen();
	test_tiling_swaps_and_resizes();
	test_untiling_restores_the_window();
	printf("\n%s\n", failures ? "FAILURES" : "all ok");
	return failures ? 1 : 0;
}
