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
#include <xkbcommon/xkbcommon-keysyms.h>
#include <linux/input-event-codes.h>

struct wm wm;
struct config config;

static const struct swc_input_mode_handler *overview_input;
static struct swc_overview_item overview_items[16];
static unsigned overview_count;
bool swc_input_mode_begin(struct swc_screen *s, const struct swc_input_mode_handler *h, void *d)
{ overview_input = h; return true; }
void swc_input_mode_end(void) { overview_input = NULL; }
bool swc_overview_begin(struct swc_screen *s, const struct swc_overview_item *items, unsigned n)
{ if (n > 16) return false; memcpy(overview_items, items, n * sizeof(*items)); overview_count = n; return true; }
void swc_overview_end(void) { overview_count = 0; }
bool swc_window_overview_geometry(struct swc_window *w, struct swc_rectangle *g)
{ return swc_window_get_geometry(w, g); }
bool chara_binding_is_overview(uint32_t mods, uint32_t key)
{ return mods == SWC_MOD_LOGO && key == XKB_KEY_Tab; }

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
static bool pinned_state[4];
void swc_window_set_pinned(struct swc_window *w, bool p)
{ unsigned i = window_index(w); if (i < 4) pinned_state[i] = p; }
void swc_window_set_minimized(struct swc_window *w, bool m) {}
void swc_window_set_workspace(struct swc_window *w, uint32_t ws) {}
void swc_workspace_set_active(struct swc_screen *s, uint32_t ws) {}
void swc_window_show(struct swc_window *w) {}
void swc_window_show_in_place(struct swc_window *w) {}
void swc_window_hide(struct swc_window *w) {}
static const struct swc_window *last_raised;
static unsigned raise_count;
void swc_window_raise(struct swc_window *w) { last_raised = w; ++raise_count; }
void swc_window_focus(struct swc_window *w) {}
static struct swc_window *closed_window;
void swc_window_close(struct swc_window *w) { closed_window = w; }
void swc_window_begin_move(struct swc_window *w) {}
void swc_window_end_move(struct swc_window *w) {}
void swc_window_begin_resize(struct swc_window *w, uint32_t e) {}
void swc_window_end_resize(struct swc_window *w) {}
static const struct swc_window_handler *window_handlers[4];
static void *window_data[4];
void swc_window_set_handler(struct swc_window *w, const struct swc_window_handler *h, void *d)
{ unsigned i = window_index(w); if (i < 4) { window_handlers[i] = h; window_data[i] = d; } }
bool swc_window_get_geometry(const struct swc_window *w, struct swc_rectangle *g)
{
	unsigned i = window_index(w);
	if (i >= 4 || !geometry_set[i])
		return false;
	*g = geometry[i];
	return true;
}
struct swc_window *swc_window_at(int32_t x, int32_t y) { return NULL; }
static bool cursor_known;
static int32_t cursor_x, cursor_y;
bool swc_cursor_position(int32_t *x, int32_t *y)
{ if (x) *x = cursor_x; if (y) *y = cursor_y; return cursor_known; }
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
	memset(pinned_state, 0, sizeof(pinned_state));
	last_raised = NULL;
	raise_count = 0;
	/* config is a zeroed global here, so the list head is only a list once
	 * it has been initialised; the rules themselves are static. */
	if (!config.rules.next)
		wl_list_init(&config.rules);
	while (!wl_list_empty(&config.rules))
		wl_list_remove(config.rules.next);
	config.tiling.enabled = true;

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
		chara_tiling_ws_init(&screens[i]);
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

/*
 * Monocle hands every window the same rectangle, so the only thing that
 * decides what you can see is which one is on top. Focusing has to raise, or
 * cycling through them changes nothing visible -- which is exactly what went
 * wrong the first time.
 */
static void
test_monocle_focus_raises(void)
{
	struct client *a, *b;

	setup();
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_set(b, true);
	chara_tiling_set_layout(&screens[0], 1, TILE_MONOCLE);
	chara_tiling_flush();
	check("in monocle both windows fill the workspace",
	      is_rect(0, 2, 26, 1916, 1052) && is_rect(1, 2, 26, 1916, 1052));

	last_raised = NULL;
	chara_focus(a);
	check("focusing one brings it to the front", last_raised == a->win);
	last_raised = NULL;
	chara_focus(b);
	check("and focusing the other brings that one", last_raised == b->win);

	/* Outside monocle the windows are side by side, and raising on focus
	 * would shuffle the stack for nothing. */
	chara_tiling_set_layout(&screens[0], 1, TILE_COLUMNS);
	chara_tiling_flush();
	last_raised = NULL;
	chara_focus(a);
	check("in a layout that does not overlap, focus leaves the stack alone",
	      last_raised == NULL);
}

static void
test_monocle_cycles_with_the_directions(void)
{
	struct client *a, *b, *cc;

	setup();
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	cc = add_client(2, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_set(b, true);
	chara_tiling_set(cc, true);
	chara_tiling_set_layout(&screens[0], 1, TILE_MONOCLE);
	chara_tiling_flush();
	wm.scr = &screens[0];

	chara_focus(a);
	check("right steps to the next window in monocle",
	      chara_focus_dir(TILE_RIGHT) && wm.cur == b);
	check("and on to the one after", chara_focus_dir(TILE_DOWN) && wm.cur == cc);
	check("the end wraps round to the start",
	      chara_focus_dir(TILE_RIGHT) && wm.cur == a);
	check("and left goes back the other way, wrapping too",
	      chara_focus_dir(TILE_LEFT) && wm.cur == cc);
}

/*
 * A launcher wants to be left out of the layout and kept above everything,
 * fullscreen windows included. Both come from its rule.
 */
static void
test_rule_can_float_and_pin(void)
{
	static struct rule rule;
	struct client *c;

	setup();
	memset(&rule, 0, sizeof(rule));
	snprintf(rule.app_id, sizeof(rule.app_id), "mylauncher");
	snprintf(rule.name, sizeof(rule.name), "launcher");
	rule.width = 500;
	rule.height = 800;
	rule.center = true;
	rule.has_tiled = true;
	rule.tiled = false;
	rule.has_pinned = true;
	rule.pinned = true;
	wl_list_insert(config.rules.prev, &rule.link);
	config.tiling.enabled = true;

	windows[0].app_id = (char *)"mylauncher";
	chara_new_window(&windows[0]);
	chara_tiling_flush();
	c = wm.cur;

	check("a rule saying tiling = false keeps the window out of the layout",
	      c && !c->tiled);
	check("pinned = true puts it above everything", c && pinned_state[0]);
	check("and its size and centring still apply",
	      is_rect(0, 710, 140, 500, 800));

	/* The window that opens next must not be affected by any of it. */
	windows[1].app_id = NULL;
	chara_new_window(&windows[1]);
	chara_tiling_flush();
	check("while an ordinary window still tiles",
	      wm.cur && wm.cur->tiled && !pinned_state[1]);
}

/*
 * Every workspace on every monitor carries its own layout, master count and
 * master ratio, so two workspaces on one screen can be arranged differently
 * and switching between them switches the arrangement with it.
 */
static void
test_each_workspace_keeps_its_own_layout(void)
{
	struct client *a, *b, *c, *d;

	setup();
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	chara_tiling_set(a, true);
	chara_tiling_set(b, true);
	chara_tiling_set_layout(&screens[0], 1, TILE_COLUMNS);
	chara_tiling_flush();
	check("workspace 1 puts its two windows side by side",
	      is_rect(0, 2, 26, 956, 1052) && is_rect(1, 962, 26, 956, 1052));

	chara_ws_go_to(&screens[0], 2);
	c = add_client(2, &screens[0]);
	d = add_client(3, &screens[0]);
	chara_tiling_set(c, true);
	chara_tiling_set(d, true);
	chara_tiling_set_layout(&screens[0], 2, TILE_ROWS);
	chara_tiling_master_count(&screens[0], 2, 2);
	chara_tiling_flush();
	check("workspace 2 of the same monitor stacks its own instead",
	      is_rect(2, 2, 26, 1916, 512) && is_rect(3, 2, 566, 1916, 512));

	/* The monitor next door is not touched by any of it. */
	chara_tiling_set_layout(&screens[1], 1, TILE_GRID);

	chara_ws_go_to(&screens[0], 1);
	chara_tiling_flush();
	check("coming back finds workspace 1 exactly as it was",
	      chara_tiling_ws(&screens[0], 1)->layout == TILE_COLUMNS &&
	      is_rect(0, 2, 26, 956, 1052) && is_rect(1, 962, 26, 956, 1052));
	check("while workspace 2 still holds its own layout",
	      chara_tiling_ws(&screens[0], 2)->layout == TILE_ROWS);
	check("and its own master count",
	      chara_tiling_ws(&screens[0], 1)->master_count == 1 &&
	      chara_tiling_ws(&screens[0], 2)->master_count == 3);
	check("and the other monitor's workspace 1 is separate again",
	      chara_tiling_ws(&screens[1], 1)->layout == TILE_GRID &&
	      chara_tiling_ws(&screens[0], 1)->layout == TILE_COLUMNS);
}

/*
 * Whether the workspace tiles is what decides how a window arrives on it. A
 * rule overrides it either way, so a mostly-floating session can still have
 * one application that always tiles, and the other way round.
 */
static void
test_default_decides_how_windows_arrive(void)
{
	static struct rule always_tiles;

	setup();
	chara_tiling_ws_enable(&screens[0], 1, false);
	chara_new_window(&windows[0]);
	chara_tiling_flush();
	check("with tiling off a new window floats", wm.cur && !wm.cur->tiled);

	chara_tiling_ws_enable(&screens[0], 1, true);
	chara_new_window(&windows[1]);
	chara_tiling_flush();
	check("with it on a new window tiles", wm.cur && wm.cur->tiled);

	/* A rule wins over whichever way the default is set. */
	setup();
	memset(&always_tiles, 0, sizeof(always_tiles));
	snprintf(always_tiles.app_id, sizeof(always_tiles.app_id), "editor");
	always_tiles.has_tiled = true;
	always_tiles.tiled = true;
	wl_list_insert(config.rules.prev, &always_tiles.link);
	chara_tiling_ws_enable(&screens[0], 1, false);

	windows[0].app_id = (char *)"editor";
	chara_new_window(&windows[0]);
	chara_tiling_flush();
	check("a rule can tile one application in a floating session",
	      wm.cur && wm.cur->tiled);

	windows[1].app_id = NULL;
	chara_new_window(&windows[1]);
	chara_tiling_flush();
	check("without taking anything else with it", wm.cur && !wm.cur->tiled);
}

/*
 * Tiling is the workspace's, not the session's: the switch takes the windows
 * already there in or out with it, the workspace next door keeps its own
 * answer, and a window sent across arrives as that workspace does things.
 */
static void
test_tiling_is_per_workspace(void)
{
	struct client *a, *b;

	setup();
	chara_tiling_ws_enable(&screens[0], 1, false);
	chara_new_window(&windows[0]);
	chara_tiling_flush();
	a = wm.cur;
	check("a window opening on a floating workspace floats", a && !a->tiled);

	chara_tiling_ws_enable(&screens[0], 1, true);
	chara_tiling_flush();
	check("turning the workspace's tiling on takes it in", a && a->tiled);
	check("and lays it out", is_rect(0, 2, 26, 1916, 1052));

	chara_tiling_ws_enable(&screens[0], 1, false);
	chara_tiling_flush();
	check("turning it off leaves it floating again", a && !a->tiled);

	/* Workspace 2 was never touched and still tiles what opens on it. */
	chara_ws_go_to(&screens[0], 2);
	chara_new_window(&windows[1]);
	chara_tiling_flush();
	b = wm.cur;
	check("the workspace next door is not touched by either",
	      chara_tiling_ws_enabled(&screens[0], 2) && b && b->tiled);

	chara_ws_move_to(1, b);
	chara_tiling_flush();
	check("a window sent to a floating workspace leaves the tiling",
	      b && !b->tiled);

	chara_ws_move_to(2, b);
	chara_tiling_flush();
	check("and one sent back to a tiling workspace joins it", b && b->tiled);
}

static void overview_point(unsigned index)
{
	cursor_known = true;
	cursor_x = wl_fixed_from_int(overview_items[index].rect.x + 3);
	cursor_y = wl_fixed_from_int(overview_items[index].rect.y + 3);
	overview_input->motion(NULL, cursor_x, cursor_y);
}

static void test_overview(void)
{
	setup();
	config.overview = (struct overview_config){ .include_minimized=true, .labels=true,
	    .inner_gap=8, .outer_gap=30 };
	chara_tiling_ws_enable(&screens[0], 1, false);
	chara_tiling_ws_enable(&screens[0], 2, false);
	chara_tiling_ws_enable(&screens[1], 1, false);
	chara_new_window(&windows[0]);
	struct client *a = wm.cur;
	chara_new_window(&windows[1]);
	struct client *b = wm.cur;
	chara_ws_move_to(2, b);
	chara_minimize(b);
	wm.scr = &screens[1];
	chara_new_window(&windows[2]);
	wm.scr = &screens[0];
	chara_focus(a);
	struct swc_rectangle saved[4];
	memcpy(saved, geometry, sizeof(saved));
	check("overview opens", chara_overview_toggle());
	check("monitor scope includes minimized and other workspaces", overview_count == 2);
	check("overview excludes another monitor", overview_items[1].window == b->win);
	chara_focus(b);
	check("ordinary focus cannot steal modal keyboard", wm.cur == a);
	check("overview does not resize clients", !memcmp(saved, geometry, sizeof(saved)));
	cursor_known = true;
	cursor_x = wl_fixed_from_int(2000); cursor_y = wl_fixed_from_int(100);
	overview_input->motion(NULL, cursor_x, cursor_y);
	check("crossing to another monitor restores its focus", wm.cur == window_data[2]);
	check("overview remains on its original monitor", chara_overview_on_screen(&screens[0]) && overview_count == 2);
	overview_input->button(NULL, BTN_LEFT);
	check("clicking the other monitor does not dismiss overview", overview_input && overview_count == 2);
	chara_ws_go_to(&screens[1], 2);
	check("workspace changes on the other monitor keep overview", overview_input && screens[1].ws == 2);
	chara_ws_go_to(&screens[1], 1);
	window_handlers[2]->request_activate(window_data[2]);
	check("other monitor can activate its windows", wm.cur == window_data[2] && overview_input);
	check("overview shortcut on other monitor leaves this one alone", chara_overview_toggle() && overview_input);
	overview_point(0);
	overview_input->key(NULL, XKB_KEY_Escape, 0);
	check("Escape removes mode and restores focus", !overview_input && !overview_count && wm.cur == a);

	config.overview.workspace = true;
	check("workspace scope opens", chara_overview_toggle());
	check("workspace scope omits other workspaces", overview_count == 1);
	chara_overview_cancel();
	config.overview.workspace = false;
	check("overview opens again", chara_overview_toggle());
	overview_point(1);
	overview_input->button(NULL, BTN_RIGHT);
	check("right click requests close without leaving overview", closed_window == b->win && overview_input);
	overview_input->key(NULL, XKB_KEY_Tab, SWC_MOD_LOGO);
	check("hover then Mod+Tab switches workspace and restores window",
	      screens[0].ws == 2 && !b->minimized && wm.cur == b && !overview_input);
	cursor_known = false;
	check("overview can reopen after a pick", chara_overview_toggle());
	cursor_known = true; cursor_x = cursor_y = 0;
	overview_input->button(NULL, BTN_LEFT);
	check("background click cancels", !overview_input && wm.cur == b);
	cursor_known = false;
	check("overview enters before forced cancellation", chara_overview_toggle());
	overview_input->cancel(NULL);
	check("forced exit removes input and rendering", !overview_input && !overview_count && !chara_overview_active());
	check("overview opens before cancelling from another output", chara_overview_toggle());
	cursor_known = true; cursor_x = wl_fixed_from_int(2000); cursor_y = wl_fixed_from_int(100);
	overview_input->motion(NULL, cursor_x, cursor_y);
	chara_overview_cancel();
	check("cancellation preserves focus on the other monitor", wm.cur == window_data[2]);
	cursor_known = false; wm.scr = &screens[0]; chara_focus(b);

	check("overview enters before new window", chara_overview_toggle());
	chara_new_window(&windows[3]);
	check("new window joins without stealing focus", overview_count == 3 && wm.cur == b);
	window_handlers[3]->destroy(window_data[3]);
	check("destroyed window leaves the plan", overview_count == 2 && overview_input);
	window_handlers[1]->destroy(window_data[1]);
	check("destroying saved focus keeps remaining overview valid", overview_count == 1 && overview_input);
	window_handlers[0]->destroy(window_data[0]);
	check("last eligible window closing exits", !overview_input && !chara_overview_active());
	window_handlers[2]->destroy(window_data[2]);
}

/* Return/KP_Enter, plain Tab/ISO_Left_Tab stepping (as opposed to the
 * Mod+Tab shortcut, which picks through chara_overview_toggle() instead),
 * the label-strip hit test in at(), and button() with no cursor position. */
static void
test_overview_keys(void)
{
	struct client *a, *b;

	setup();
	cursor_known = false;
	config.overview = (struct overview_config){ .include_minimized = true,
	    .labels = true, .inner_gap = 8, .outer_gap = 30 };
	geometry[0] = (struct swc_rectangle){0, 0, 200, 200}; geometry_set[0] = true;
	geometry[1] = (struct swc_rectangle){0, 0, 200, 200}; geometry_set[1] = true;
	a = add_client(0, &screens[0]);
	b = add_client(1, &screens[0]);
	chara_focus(a);

	check("overview opens for the key tests", chara_overview_toggle());
	check("focus starts the selection", overview_items[0].highlighted);

	overview_input->key(NULL, XKB_KEY_Tab, 0);
	check("plain Tab steps the selection forward", overview_items[1].highlighted);
	overview_input->key(NULL, XKB_KEY_Tab, 0);
	check("Tab wraps back round", overview_items[0].highlighted);
	overview_input->key(NULL, XKB_KEY_Tab, SWC_MOD_SHIFT);
	check("Shift+Tab steps backward", overview_items[1].highlighted);
	overview_input->key(NULL, XKB_KEY_ISO_Left_Tab, 0);
	check("ISO_Left_Tab also steps backward", overview_items[0].highlighted);

	struct swc_rectangle br = overview_items[1].rect;
	cursor_known = true;
	cursor_x = wl_fixed_from_int(br.x + 3);
	cursor_y = wl_fixed_from_int(br.y + (int32_t)br.height +
	    (int32_t)overview_items[1].label_height / 2);
	overview_input->motion(NULL, cursor_x, cursor_y);
	check("clicking in the label strip below a card selects it",
	      overview_items[1].highlighted);

	cursor_known = false;
	closed_window = NULL;
	overview_input->button(NULL, BTN_LEFT);
	check("button() with no cursor position is a no-op",
	      overview_input && closed_window == NULL && overview_items[1].highlighted);

	raise_count = 0;
	overview_input->key(NULL, XKB_KEY_Return, 0);
	check("Return picks the selected window",
	      !overview_input && wm.cur == b && raise_count == 1);

	check("overview reopens for KP_Enter", chara_overview_toggle());
	raise_count = 0;
	overview_input->key(NULL, XKB_KEY_KP_Enter, 0);
	check("KP_Enter also picks the selected window",
	      !overview_input && wm.cur == b && raise_count == 1);
}

/* The arrow/hjkl handler in key(), including its calloc/tile_neighbour/free
 * round trip and the "no neighbour that way" boundary. A screen too narrow
 * for two columns forces the packer to stack the cards in one column instead
 * of side by side, so up/down has a neighbour and left/right does not. */
static void
test_overview_directional(void)
{
	struct client *a;

	setup();
	cursor_known = false;
	config.overview = (struct overview_config){ .include_minimized = true,
	    .inner_gap = 8, .outer_gap = 30 };
	swc_screens[0].usable_geometry = (struct swc_rectangle){0, 0, 460, 1080};
	geometry[0] = (struct swc_rectangle){0, 0, 200, 200}; geometry_set[0] = true;
	geometry[1] = (struct swc_rectangle){0, 0, 200, 200}; geometry_set[1] = true;
	a = add_client(0, &screens[0]);
	add_client(1, &screens[0]);
	chara_focus(a);

	check("overview opens for the directional tests", chara_overview_toggle());
	check("too narrow for a second column stacks the cards vertically",
	      overview_items[0].rect.x == overview_items[1].rect.x &&
	      overview_items[1].rect.y > overview_items[0].rect.y);
	check("focus starts the selection", overview_items[0].highlighted);

	overview_input->key(NULL, XKB_KEY_Left, 0);
	check("left has no neighbour in a single column", overview_items[0].highlighted);
	overview_input->key(NULL, XKB_KEY_h, 0);
	check("h has no neighbour either", overview_items[0].highlighted);

	overview_input->key(NULL, XKB_KEY_Down, 0);
	check("down moves to the card below", overview_items[1].highlighted);
	overview_input->key(NULL, XKB_KEY_j, 0);
	check("j has no further neighbour below", overview_items[1].highlighted);

	overview_input->key(NULL, XKB_KEY_Right, 0);
	check("right has no neighbour in a single column", overview_items[1].highlighted);
	overview_input->key(NULL, XKB_KEY_l, 0);
	check("l has no neighbour either", overview_items[1].highlighted);

	overview_input->key(NULL, XKB_KEY_Up, 0);
	check("up moves back to the top card", overview_items[0].highlighted);
	overview_input->key(NULL, XKB_KEY_k, 0);
	check("k has no further neighbour above", overview_items[0].highlighted);

	chara_overview_cancel();
}

/* The extreme-count reclaim loop: labels are dropped first, then the gaps
 * shrink, before it gives up and cancels rather than looping forever. */
static void
test_overview_reclaim(void)
{
	struct client *a;

	setup();
	cursor_known = false;
	config.overview = (struct overview_config){ .include_minimized = true,
	    .labels = true, .inner_gap = 8, .outer_gap = 30 };
	swc_screens[0].usable_geometry = (struct swc_rectangle){0, 0, 1920, 80};
	geometry[0] = (struct swc_rectangle){0, 0, 200, 200}; geometry_set[0] = true;
	a = add_client(0, &screens[0]);
	chara_focus(a);

	check("a footer that does not fit is reclaimed rather than failing",
	      chara_overview_toggle());
	check("the label strip was dropped to make room",
	      overview_items[0].label_height == 0);
	chara_overview_cancel();

	swc_screens[0].usable_geometry = (struct swc_rectangle){0, 0, 0, 1080};
	check("an area that can never fit ends the overview instead of looping forever",
	      !chara_overview_toggle());
	check("the failed rebuild leaves overview inactive",
	      !chara_overview_active() && overview_input == NULL);
}

/*
 * Clipboard tools such as wl-copy, which micro runs on every copy, paste and
 * mouse selection, open a throwaway window to get keyboard focus and close it
 * straight away. Focus has to go back to the window that had it, not to
 * whichever window happens to have been opened first -- which used to throw
 * the user out of their editor and into their browser.
 */
static void
test_closing_returns_focus_to_the_last_window(void)
{
	struct client *browser, *editor, *other;

	setup();
	chara_tiling_ws_enable(&screens[0], 1, false);
	chara_new_window(&windows[0]);
	browser = wm.cur;
	chara_new_window(&windows[1]);
	chara_new_window(&windows[2]);
	other = wm.cur;
	editor = window_data[1];
	chara_focus(browser);
	chara_focus(other);
	chara_focus(editor);

	chara_new_window(&windows[3]);
	window_handlers[3]->destroy(window_data[3]);
	check("closing a window gives focus back to the one before it",
	      wm.cur == editor);

	chara_minimize(editor);
	check("and minimizing one hands it to the last one used",
	      wm.cur == other);
}

int
main(void)
{
	test_overview();
	test_overview_keys();
	test_overview_directional();
	test_overview_reclaim();
	test_moving_clears_the_old_monitor();
	test_closing_clears_every_monitor();
	test_fullscreen_monitor_choice();
	test_fullscreen_is_undecorated();
	test_tiling_places_windows();
	test_tiling_survives_fullscreen();
	test_tiling_is_per_workspace();
	test_tiling_swaps_and_resizes();
	test_untiling_restores_the_window();
	test_monocle_focus_raises();
	test_monocle_cycles_with_the_directions();
	test_rule_can_float_and_pin();
	test_each_workspace_keeps_its_own_layout();
	test_default_decides_how_windows_arrive();
	test_closing_returns_focus_to_the_last_window();
	printf("\n%s\n", failures ? "FAILURES" : "all ok");
	return failures ? 1 : 0;
}
