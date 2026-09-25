#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "config.h"
#include "overview.h"

static struct {
	struct screen *screen;
	struct ov_item *layout;
	struct swc_overview_item *draw;
	unsigned count;
	uint32_t selected, saved_focus;
	bool rebuilding;
	/* What the current layout was arranged from. Every geometry change of
	 * every window asks for a refresh, and most change nothing the packing
	 * reads; those reuse the rectangles instead of packing again. */
	struct swc_rectangle area;
	int32_t gap, outer;
	uint32_t footer, footer_used;
} overview;

bool chara_overview_active(void) { return overview.screen != NULL; }
bool chara_overview_on_screen(const struct screen *screen)
{ return screen && overview.screen == screen; }

static struct client *by_id(uint32_t id)
{
	struct client *c;
	wl_list_for_each(c, &wm.clients, link) if (c->id == id) return c;
	return NULL;
}
static bool eligible(const struct client *c)
{
	return c->scr == overview.screen &&
	    (!config.overview.workspace || c->ws == overview.screen->ws) &&
	    (config.overview.include_minimized || !c->minimized);
}

void chara_overview_cancel(void)
{
	if (!overview.screen) return;
	struct screen *screen = overview.screen;
	struct screen *active = chara_active_screen();
	struct client *saved = by_id(overview.saved_focus);
	/* Closing or rebuilding overview must not steal focus from another output. */
	if (active && active != screen) {
		screen = active;
		saved = wm.cur && wm.cur->scr == active ? wm.cur : NULL;
	}
	overview.screen = NULL;
	swc_input_mode_end();
	swc_overview_end();
	free(overview.layout); free(overview.draw);
	overview.layout = NULL; overview.draw = NULL; overview.count = 0;
	if (saved && saved->visible && !saved->minimized) chara_focus(saved);
	else chara_focus(chara_first_on(screen));
}

static bool publish(void)
{
	for (unsigned i = 0; i < overview.count; ++i)
		overview.draw[i].highlighted = overview.layout[i].id == overview.selected;
	return swc_overview_begin(overview.screen->scr, overview.draw, overview.count);
}

void chara_overview_refresh(void)
{
	if (!overview.screen || overview.rebuilding) return;
	overview.rebuilding = true;
	struct client *c;
	unsigned n = 0;
	wl_list_for_each(c, &wm.clients, link) if (eligible(c)) ++n;
	struct ov_item *layout = n ? calloc(n, sizeof(*layout)) : NULL;
	struct swc_overview_item *draw = n ? calloc(n, sizeof(*draw)) : NULL;
	if (!layout || !draw) goto fail;
	unsigned i = 0;
	bool selected_found = false;
	wl_list_for_each(c, &wm.clients, link) {
		if (!eligible(c)) continue;
		struct swc_rectangle source;
		if (!swc_window_overview_geometry(c->win, &source))
			source = (struct swc_rectangle){c->x, c->y, c->width, c->height};
		layout[i] = (struct ov_item){ .id = c->id, .src_width = source.width,
		                             .src_height = source.height };
		draw[i] = (struct swc_overview_item){ .window = c->win, .source = source,
		    .minimized = c->minimized != 0,
		    .color = config.values.ring_count ? config.values.rings[0].focused : 0xfffabd2f };
		if (c->id == overview.selected) selected_found = true;
		++i;
	}
	if (!selected_found) overview.selected = layout[0].id;
	int gap = config.overview.inner_gap, outer = config.overview.outer_gap;
	unsigned footer = config.overview.labels ? 24 : 0;
	struct swc_rectangle area = overview.screen->scr->usable_geometry;
	bool same = overview.layout && n == overview.count &&
	    !memcmp(&area, &overview.area, sizeof(area)) &&
	    gap == overview.gap && outer == overview.outer &&
	    footer == overview.footer;
	for (i = 0; same && i < n; ++i)
		same = layout[i].id == overview.layout[i].id &&
		       layout[i].src_width == overview.layout[i].src_width &&
		       layout[i].src_height == overview.layout[i].src_height;
	if (same) {
		for (i = 0; i < n; ++i)
			layout[i].rect = overview.layout[i].rect;
		footer = overview.footer_used;
	} else {
		overview.area = area;
		overview.gap = gap;
		overview.outer = outer;
		overview.footer = footer;
		/* At extreme counts, reclaim labels and spacing before giving up.
		 * No window is omitted. At least one background pixel remains at
		 * the edge. */
		while (!ov_arrange(layout, n, area, gap, outer, footer)) {
			if (footer) footer = 0;
			else if (gap) gap /= 2;
			else if (outer > 1) outer /= 2;
			else goto fail;
		}
		overview.footer_used = footer;
	}
	for (i = 0; i < n; ++i) {
		draw[i].rect = layout[i].rect;
		draw[i].label_height = footer;
	}
	free(overview.layout); free(overview.draw);
	overview.layout = layout; overview.draw = draw; overview.count = n;
	overview.rebuilding = false;
	if (!publish()) chara_overview_cancel();
	return;
fail:
	free(layout); free(draw);
	overview.rebuilding = false;
	chara_overview_cancel();
}

static int at(int32_t x, int32_t y)
{
	for (unsigned i = 0; i < overview.count; ++i) {
		struct swc_rectangle r = overview.draw[i].rect;
		r.height += overview.draw[i].label_height;
		if (x >= r.x && y >= r.y && (int64_t)x < (int64_t)r.x + r.width &&
		    (int64_t)y < (int64_t)r.y + r.height) return i;
	}
	return -1;
}
static void select_index(int index)
{
	uint32_t id = index >= 0 ? overview.layout[index].id : 0;
	if (id == overview.selected) return;
	overview.selected = id;
	if (!publish()) chara_overview_cancel();
}
static void pick(uint32_t id)
{
	struct client *c = by_id(id);
	if (!c) return;
	chara_overview_cancel();
	if (c->scr) {
		wm.scr = c->scr;
		chara_ws_go_to(c->scr, c->ws);
	}
	if (c->minimized) chara_restore(c);
	swc_window_raise(c->win);
	chara_focus(c);
}
static void motion(void *data, int32_t x, int32_t y)
{
	struct screen *screen = chara_screen_at(wl_fixed_to_int(x), wl_fixed_to_int(y));
	if (screen && screen != overview.screen) {
		if (wm.scr != screen) {
			wm.scr = screen;
			chara_focus(chara_first_on(screen));
		}
		return;
	}
	wm.scr = overview.screen;
	select_index(at(wl_fixed_to_int(x), wl_fixed_to_int(y)));
}
static void button(void *data, uint32_t value)
{
	int32_t x, y;
	if (!swc_cursor_position(&x, &y)) return;
	if (!chara_overview_on_screen(chara_screen_at(wl_fixed_to_int(x), wl_fixed_to_int(y)))) return;
	int index = at(wl_fixed_to_int(x), wl_fixed_to_int(y));
	if (value == BTN_LEFT) {
		if (index < 0) chara_overview_cancel();
		else pick(overview.layout[index].id);
	} else if (value == BTN_RIGHT && index >= 0) {
		struct client *c = by_id(overview.layout[index].id);
		if (c) swc_window_close(c->win);
	}
}
static void key(void *data, uint32_t sym, uint32_t mods)
{
	if (sym == XKB_KEY_Escape) { chara_overview_cancel(); return; }
	if (chara_binding_is_overview(mods, sym)) { chara_overview_toggle(); return; }
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { pick(overview.selected); return; }
	unsigned from = 0;
	for (unsigned i = 0; i < overview.count; ++i)
		if (overview.layout[i].id == overview.selected) from = i;
	if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
		int step = (mods & SWC_MOD_SHIFT) || sym == XKB_KEY_ISO_Left_Tab ? -1 : 1;
		select_index((from + overview.count + step) % overview.count); return;
	}
	enum tile_dir dir;
	switch (sym) {
	case XKB_KEY_Left: case XKB_KEY_h: dir = TILE_LEFT; break;
	case XKB_KEY_Right: case XKB_KEY_l: dir = TILE_RIGHT; break;
	case XKB_KEY_Up: case XKB_KEY_k: dir = TILE_UP; break;
	case XKB_KEY_Down: case XKB_KEY_j: dir = TILE_DOWN; break;
	default: return;
	}
	struct tile_item *tiles = calloc(overview.count, sizeof(*tiles));
	if (!tiles) return;
	for (unsigned i = 0; i < overview.count; ++i) tiles[i].rect = overview.layout[i].rect;
	int next = tile_neighbour(tiles, overview.count, from, dir);
	free(tiles);
	if (next >= 0) select_index(next);
}
static void cancelled(void *data) { chara_overview_cancel(); }
static const struct swc_input_mode_handler input = {
	.key = key, .motion = motion, .button = button, .cancel = cancelled,
};

bool chara_overview_toggle(void)
{
	struct screen *screen = chara_active_screen();
	if (overview.screen) {
		/* The shortcut belongs to the monitor under the pointer. A second
		 * monitor must not dismiss or select a window in this overview. */
		if (screen == overview.screen) {
			if (overview.selected) pick(overview.selected);
			else chara_overview_cancel();
		}
		return true;
	}
	if (!screen || !screen->scr || wm.grab.active) return false;
	chara_tiling_flush();
	if (!swc_input_mode_begin(screen->scr, &input, NULL)) return false;
	overview.screen = screen;
	overview.saved_focus = wm.cur ? wm.cur->id : 0;
	overview.selected = overview.saved_focus;
	chara_overview_refresh();
	return overview.screen != NULL;
}
