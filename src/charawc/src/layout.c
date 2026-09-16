#include <stdlib.h>
#include <string.h>

#include "config.h"

/*
 * Slice i of n out of r along axis.
 *
 * Boundaries are computed from the full extent rather than by repeated
 * halving, so the slices tile the area exactly: no gap, no overlap, and at
 * most a pixel between the largest and the smallest.
 */
static struct swc_rectangle
slice(struct swc_rectangle r, enum split_axis axis, unsigned i, unsigned n)
{
	uint64_t extent = axis == SPLIT_VERTICAL ? r.width : r.height;
	int32_t start = (int32_t)(extent * i / n);
	int32_t end = (int32_t)(extent * (i + 1) / n);

	if (axis == SPLIT_VERTICAL) {
		r.x += start;
		r.width = (uint32_t)(end - start);
	} else {
		r.y += start;
		r.height = (uint32_t)(end - start);
	}
	return r;
}

/*
 * Geometry of window i of n.
 *
 * split: every window gets an equal share of the screen along the configured
 *        axis, so three windows are thirds rather than a half and two
 *        quarters.
 * quad:  rows are always horizontal bands and the configured axis divides
 *        each row. The first two windows share the screen, the third takes
 *        the whole bottom band, and the fourth splits that band. With a
 *        vertical axis this gives a 2x2 box.
 */
static struct swc_rectangle
tile_geometry(struct swc_rectangle area, unsigned i, unsigned n)
{
	if (n <= 1)
		return area;

	if (config.layout.mode == LAYOUT_SPLIT)
		return slice(area, config.layout.axis, i, n);

	/* LAYOUT_QUAD */
	if (n == 2)
		return slice(area, config.layout.axis, i, 2);

	struct swc_rectangle band = slice(area, SPLIT_HORIZONTAL, i < 2 ? 0 : 1, 2);
	if (i < 2)
		return slice(band, config.layout.axis, i, 2);
	if (n == 3)
		return band;
	return slice(band, config.layout.axis, i - 2, 2);
}

/* Windows that take part in tiling on this monitor and workspace. */
static unsigned
collect(struct screen *s, uint8_t ws, struct client **out, unsigned max)
{
	struct client *c;
	unsigned n = 0;

	wl_list_for_each(c, &wm.tiles, tile_link) {
		if (c->scr != s || c->ws != ws || c->minimized || c->fullscreen)
			continue;
		if (n < max)
			out[n] = c;
		++n;
	}
	return n < max ? n : max;
}

void
chara_layout_apply(struct screen *s, uint8_t ws)
{
	struct client *tiled[CHARA_WORKSPACES * 16];
	unsigned n = collect(s, ws, tiled, sizeof(tiled) / sizeof(*tiled));
	if (!n || !s->scr)
		return;

	struct swc_rectangle area = s->scr->usable_geometry;
	int32_t pad = chara_border_width();

	for (unsigned i = 0; i < n; ++i) {
		struct client *c = tiled[i];
		struct swc_rectangle g = tile_geometry(area, i, n);
		/* A titlebar sits above the content, so it eats into the top. */
		int32_t top = pad + chara_titlebar_height(c);

		g.x += pad;
		g.y += top;
		g.width = g.width > (uint32_t)(2 * pad) ? g.width - 2 * pad : 1;
		g.height = g.height > (uint32_t)(top + pad) ? g.height - top - pad : 1;

		swc_window_set_geometry(c->win, &g);
		c->x = g.x;
		c->y = g.y;
		c->width = g.width;
		c->height = g.height;
	}
}

void
chara_layout_all(void)
{
	struct screen *s;
	wl_list_for_each(s, &wm.screens, link)
		chara_layout_apply(s, s->ws);
}

/* Number of windows already tiled on a monitor and workspace. */
static unsigned
tiled_count(struct screen *s, uint8_t ws)
{
	struct client *c;
	unsigned n = 0;

	wl_list_for_each(c, &wm.tiles, tile_link)
		if (c->scr == s && c->ws == ws)
			++n;
	return n;
}

bool
chara_layout_admit(struct client *c)
{
	if (config.layout.mode == LAYOUT_FLOATING)
		return false;
	if (tiled_count(c->scr, c->ws) >= config.layout.max)
		return false; /* over the limit, the window opens floating */

	wl_list_insert(wm.tiles.prev, &c->tile_link);
	c->tiled = true;
	swc_window_set_tiled(c->win);
	return true;
}

void
chara_layout_drop(struct client *c)
{
	if (!c->tiled)
		return;
	wl_list_remove(&c->tile_link);
	c->tiled = false;
	chara_layout_apply(c->scr, c->ws);
}

void
chara_layout_set_floating(struct client *c, bool floating)
{
	if (floating == !c->tiled)
		return;

	if (floating) {
		chara_layout_drop(c);
		swc_window_set_stacked(c->win);
		struct swc_rectangle g = {
			.x = c->x, .y = c->y,
			.width = c->width ? c->width : 640,
			.height = c->height ? c->height : 480,
		};
		swc_window_set_geometry(c->win, &g);
	} else if (chara_layout_admit(c)) {
		chara_layout_apply(c->scr, c->ws);
	}
}
