/*
 * Exercises src/tiling_layout.c on its own.
 *
 * The layout engine touches no compositor, so unlike focus_test this one links
 * against nothing and needs no stubs: it is arithmetic all the way down, and
 * arithmetic is the part of tiling that is worth being sure about.
 *
 * The invariant everything else rests on is that the cells tile the area
 * exactly. With no gaps that is checkable outright -- paint every cell into a
 * coverage map and insist each pixel of the area was painted exactly once --
 * and that is what test_exact_without_gaps does, for every layout, at sizes
 * whose dimensions are prime so that rounding has nowhere to hide.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiling.h"

static int failures;

static void
check(const char *what, bool ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		++failures;
}

/* ------------------------------------------------------------ helpers */

static struct tile_item items[TILE_MAX_WINDOWS];

static void
reset(unsigned n)
{
	for (unsigned i = 0; i < n; ++i)
		tile_item_init(&items[i]);
}

static struct tile_ws
ws_of(enum tile_layout layout)
{
	struct tile_ws ws;

	tile_ws_init(&ws);
	ws.layout = layout;
	return ws;
}

static bool
overlaps(struct swc_rectangle a, struct swc_rectangle b)
{
	return a.x < b.x + (int32_t)b.width && b.x < a.x + (int32_t)a.width &&
	       a.y < b.y + (int32_t)b.height && b.y < a.y + (int32_t)a.height;
}

static bool
contains(struct swc_rectangle outer, struct swc_rectangle inner)
{
	return inner.x >= outer.x && inner.y >= outer.y &&
	       inner.x + (int32_t)inner.width <= outer.x + (int32_t)outer.width &&
	       inner.y + (int32_t)inner.height <= outer.y + (int32_t)outer.height;
}

static struct swc_rectangle
bounding_box(unsigned n)
{
	int32_t x0 = items[0].rect.x, y0 = items[0].rect.y;
	int32_t x1 = x0 + (int32_t)items[0].rect.width;
	int32_t y1 = y0 + (int32_t)items[0].rect.height;

	for (unsigned i = 1; i < n; ++i) {
		struct swc_rectangle *r = &items[i].rect;

		if (r->x < x0) x0 = r->x;
		if (r->y < y0) y0 = r->y;
		if (r->x + (int32_t)r->width > x1) x1 = r->x + (int32_t)r->width;
		if (r->y + (int32_t)r->height > y1) y1 = r->y + (int32_t)r->height;
	}
	return (struct swc_rectangle){ x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
}

static bool
none_overlap(unsigned n)
{
	for (unsigned i = 0; i < n; ++i)
		for (unsigned j = i + 1; j < n; ++j)
			if (overlaps(items[i].rect, items[j].rect))
				return false;
	return true;
}

static bool
all_positive(unsigned n)
{
	for (unsigned i = 0; i < n; ++i)
		if (!items[i].rect.width || !items[i].rect.height)
			return false;
	return true;
}

/*
 * Every pixel of `area` painted exactly once. Only meaningful without gaps,
 * where the cells are supposed to account for the whole area.
 */
static bool
covers_exactly(unsigned n, struct swc_rectangle area)
{
	unsigned char *map = calloc((size_t)area.width * area.height, 1);
	bool ok = true;

	if (!map)
		return false;
	for (unsigned i = 0; i < n && ok; ++i) {
		struct swc_rectangle *r = &items[i].rect;

		for (int32_t y = r->y; y < r->y + (int32_t)r->height && ok; ++y) {
			for (int32_t x = r->x; x < r->x + (int32_t)r->width; ++x) {
				size_t at;

				if (x < area.x || y < area.y ||
				    x >= area.x + (int32_t)area.width ||
				    y >= area.y + (int32_t)area.height) {
					ok = false;   /* painted outside the area */
					break;
				}
				at = (size_t)(y - area.y) * area.width + (x - area.x);
				if (map[at]++) {
					ok = false;   /* painted twice */
					break;
				}
			}
		}
	}
	for (size_t at = 0; ok && at < (size_t)area.width * area.height; ++at)
		if (!map[at])
			ok = false;           /* never painted */
	free(map);
	return ok;
}

/* ------------------------------------------------------------- layout */

static void
test_exact_without_gaps(void)
{
	/* Prime dimensions, so no split divides evenly and the rounding has to
	 * be accounted for rather than happening to come out. */
	static const struct swc_rectangle areas[] = {
		{ 0, 0, 199, 149 },
		{ 37, 11, 211, 157 },
		{ -60, -40, 101, 103 },
	};
	bool ok = true;

	for (unsigned layout = 0; layout < TILE_LAYOUT_LAST && ok; ++layout) {
		struct tile_ws ws = ws_of((enum tile_layout)layout);

		if (layout == TILE_MONOCLE)
			continue;   /* monocle stacks on purpose; see its own test */
		for (unsigned a = 0; a < sizeof(areas) / sizeof(*areas) && ok; ++a) {
			for (unsigned n = 1; n <= 17 && ok; ++n) {
				for (unsigned nm = 1; nm <= 3 && ok; ++nm) {
					ws.master_count = nm;
					reset(n);
					tile_arrange(&ws, items, n, areas[a], 0, 0);
					ok = covers_exactly(n, areas[a]) && none_overlap(n);
					if (!ok)
						printf("   (%s n=%u nm=%u area %dx%d)\n",
						       tile_layout_name(ws.layout), n, nm,
						       areas[a].width, areas[a].height);
				}
			}
		}
	}
	check("every layout tiles the area exactly, with no gaps", ok);
}

static void
test_exact_with_uneven_weights(void)
{
	struct swc_rectangle area = { 13, 7, 997, 601 };
	bool ok = true;

	for (unsigned layout = 0; layout < TILE_LAYOUT_LAST && ok; ++layout) {
		struct tile_ws ws = ws_of((enum tile_layout)layout);

		if (layout == TILE_MONOCLE)
			continue;
		for (unsigned n = 2; n <= 13 && ok; ++n) {
			reset(n);
			/* Weights no split will land on a round number of pixels. */
			for (unsigned i = 0; i < n; ++i) {
				items[i].main = 0.3 + (double)((i * 7) % 11) / 3.0;
				items[i].cross = 0.2 + (double)((i * 5) % 9) / 4.0;
			}
			ws.master_ratio = 0.37;
			tile_arrange(&ws, items, n, area, 0, 0);
			ok = covers_exactly(n, area) && none_overlap(n);
			if (!ok)
				printf("   (%s n=%u)\n", tile_layout_name(ws.layout), n);
		}
	}
	check("and does it with weights that never divide evenly", ok);
}

static void
test_gaps_stay_inside(void)
{
	struct swc_rectangle area = { 100, 50, 1720, 1030 };
	bool ok = true;

	for (unsigned layout = 0; layout < TILE_LAYOUT_LAST && ok; ++layout) {
		struct tile_ws ws = ws_of((enum tile_layout)layout);

		for (unsigned n = 1; n <= 13 && ok; ++n) {
			struct swc_rectangle inner = { area.x + 10, area.y + 10,
			                               area.width - 20, area.height - 20 };
			reset(n);
			tile_arrange(&ws, items, n, area, 8, 10);
			ok = none_overlap(n) || ws.layout == TILE_MONOCLE;
			for (unsigned i = 0; i < n && ok; ++i)
				ok = contains(inner, items[i].rect);
			if (ok && n > 1) {
				/* The cells reach every side of the inner area: the outer
				 * gap is exactly what was asked for, not more. */
				struct swc_rectangle box = bounding_box(n);
				ok = box.x == inner.x && box.y == inner.y &&
				     box.width == inner.width && box.height == inner.height;
			}
			if (!ok)
				printf("   (%s n=%u)\n", tile_layout_name(ws.layout), n);
		}
	}
	check("gaps come out of the cells, never out of the area", ok);
}

static void
test_gaps_give_way(void)
{
	/* A workspace far too small for the gaps it was asked for: the windows
	 * must survive it, even if the gaps do not. */
	struct swc_rectangle area = { 0, 0, 40, 30 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);
	bool ok;

	reset(12);
	tile_arrange(&ws, items, 12, area, 20, 20);
	ok = all_positive(12) && none_overlap(12);
	check("windows keep a pixel when the gaps cannot fit", ok);
}

static void
test_columns_and_rows(void)
{
	struct swc_rectangle area = { 0, 0, 1000, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);

	reset(4);
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("four columns are quarters",
	      items[0].rect.width == 250 && items[3].rect.width == 250 &&
	      items[0].rect.x == 0 && items[3].rect.x == 750 &&
	      items[2].rect.height == 600);

	ws.layout = TILE_ROWS;
	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("three rows are thirds, to the pixel",
	      items[0].rect.height == 200 && items[1].rect.height == 200 &&
	      items[2].rect.height == 200 && items[2].rect.y == 400 &&
	      items[1].rect.width == 1000);

	reset(3);
	tile_arrange(&ws, items, 3, (struct swc_rectangle){ 0, 0, 1000, 601 }, 0, 0);
	check("an odd height still adds up",
	      items[0].rect.height + items[1].rect.height + items[2].rect.height == 601);
}

static void
test_master(void)
{
	struct swc_rectangle area = { 0, 0, 1000, 600 };
	struct tile_ws ws = ws_of(TILE_MASTER);

	reset(1);
	tile_arrange(&ws, items, 1, area, 0, 0);
	check("one window fills the workspace",
	      items[0].rect.width == 1000 && items[0].rect.height == 600);

	reset(3);
	ws.master_ratio = 0.6;
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("master takes its share and the stack splits the rest",
	      items[0].rect.x == 0 && items[0].rect.width == 600 &&
	      items[0].rect.height == 600 &&
	      items[1].rect.x == 600 && items[1].rect.width == 400 &&
	      items[1].rect.height == 300 && items[2].rect.y == 300);

	reset(4);
	ws.master_count = 2;
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("two masters share the master column",
	      items[0].rect.height == 300 && items[1].rect.y == 300 &&
	      items[1].rect.width == 600 && items[2].rect.x == 600);

	reset(2);
	ws.master_count = 5;   /* more masters than windows */
	tile_arrange(&ws, items, 2, area, 0, 0);
	check("with nothing in the stack the masters take the whole workspace",
	      items[0].rect.width == 1000 && items[1].rect.width == 1000 &&
	      items[0].rect.height == 300 && items[1].rect.y == 300);

	reset(3);
	ws.master_count = 1;
	ws.master_ratio = 0.6;
	ws.master_side = TILE_SIDE_RIGHT;
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("the master group can sit on the right",
	      items[0].rect.x == 400 && items[0].rect.width == 600 &&
	      items[1].rect.x == 0 && items[1].rect.width == 400);

	reset(3);
	ws.master_side = TILE_SIDE_TOP;
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("or across the top, with the stack side by side below",
	      items[0].rect.y == 0 && items[0].rect.height == 360 &&
	      items[0].rect.width == 1000 &&
	      items[1].rect.y == 360 && items[1].rect.width == 500 &&
	      items[2].rect.x == 500);
}

static void
test_grid(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_GRID);

	reset(4);
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("four windows make a two by two grid",
	      items[0].rect.width == 450 && items[0].rect.height == 300 &&
	      items[1].rect.x == 450 && items[2].rect.y == 300 &&
	      items[3].rect.x == 450 && items[3].rect.y == 300);

	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("three fill the top row first and stretch the last one",
	      items[0].rect.width == 450 && items[1].rect.x == 450 &&
	      items[2].rect.y == 300 && items[2].rect.width == 900);

	reset(6);
	tile_arrange(&ws, items, 6, area, 0, 0);
	check("six make three across and two down",
	      items[0].rect.width == 300 && items[2].rect.x == 600 &&
	      items[3].rect.y == 300 && items[5].rect.x == 600);

	reset(2);
	tile_arrange(&ws, items, 2, area, 0, 0);
	check("two sit side by side rather than stacked",
	      items[0].rect.width == 450 && items[0].rect.height == 600 &&
	      items[1].rect.x == 450);
}

static void
test_monocle(void)
{
	struct swc_rectangle area = { 20, 30, 800, 400 };
	struct tile_ws ws = ws_of(TILE_MONOCLE);
	bool ok = true;

	reset(5);
	tile_arrange(&ws, items, 5, area, 8, 10);
	for (unsigned i = 0; i < 5; ++i)
		ok = ok && items[i].rect.x == 30 && items[i].rect.y == 40 &&
		     items[i].rect.width == 780 && items[i].rect.height == 380;
	check("monocle gives every window the whole area, outer gap aside", ok);
	check("and nothing is anyone's neighbour there",
	      tile_neighbour(items, 5, 0, TILE_RIGHT) < 0 &&
	      tile_neighbour(items, 5, 2, TILE_DOWN) < 0);
}

/* -------------------------------------------------------- neighbours */

static void
test_neighbours(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);

	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("in columns the neighbours are the columns either side",
	      tile_neighbour(items, 3, 1, TILE_LEFT) == 0 &&
	      tile_neighbour(items, 3, 1, TILE_RIGHT) == 2 &&
	      tile_neighbour(items, 3, 0, TILE_LEFT) < 0 &&
	      tile_neighbour(items, 3, 2, TILE_RIGHT) < 0);
	check("and nothing is above or below them",
	      tile_neighbour(items, 3, 1, TILE_UP) < 0 &&
	      tile_neighbour(items, 3, 1, TILE_DOWN) < 0);

	ws.layout = TILE_GRID;
	reset(4);
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("in a grid they are the cells sharing an edge",
	      tile_neighbour(items, 4, 0, TILE_RIGHT) == 1 &&
	      tile_neighbour(items, 4, 0, TILE_DOWN) == 2 &&
	      tile_neighbour(items, 4, 3, TILE_LEFT) == 2 &&
	      tile_neighbour(items, 4, 3, TILE_UP) == 1);
	check("never the one diagonally across",
	      tile_neighbour(items, 4, 0, TILE_LEFT) < 0 &&
	      tile_neighbour(items, 4, 0, TILE_UP) < 0);

	/* Master on the left, three in the stack: going right from the master
	 * lands on the stack window across from its middle, and coming back
	 * from any of them returns to the master. */
	ws.layout = TILE_MASTER;
	ws.master_count = 1;
	reset(4);
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("from a master spanning the stack, right picks the middle one",
	      tile_neighbour(items, 4, 0, TILE_RIGHT) == 2);
	check("and every stack window comes back to the master",
	      tile_neighbour(items, 4, 1, TILE_LEFT) == 0 &&
	      tile_neighbour(items, 4, 2, TILE_LEFT) == 0 &&
	      tile_neighbour(items, 4, 3, TILE_LEFT) == 0);
	check("the stack is its own column top to bottom",
	      tile_neighbour(items, 4, 1, TILE_DOWN) == 2 &&
	      tile_neighbour(items, 4, 3, TILE_UP) == 2 &&
	      tile_neighbour(items, 4, 1, TILE_UP) < 0);

	check("a point lands in the cell that holds it",
	      tile_at(items, 4, 10, 10) == 0 &&
	      tile_at(items, 4, 800, 10) == 1 &&
	      tile_at(items, 4, 800, 590) == 3 &&
	      tile_at(items, 4, -5, 0) < 0);
}

/* ------------------------------------------------------------ resize */

static void
test_resize_columns(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);
	uint32_t before;

	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	before = items[2].rect.width;
	check("growing a column's right edge moves that fence",
	      tile_resize(&ws, items, 3, 1, TILE_RIGHT, 60, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("by the pixels asked for",
	      items[1].rect.width == 360 && items[1].rect.x == 300);
	check("out of its neighbour and nobody else",
	      items[2].rect.width == before - 60 && items[0].rect.width == 300 &&
	      items[0].rect.width + items[1].rect.width + items[2].rect.width == 900);

	check("and back again",
	      tile_resize(&ws, items, 3, 1, TILE_RIGHT, -60, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("leaves the columns as they were",
	      items[0].rect.width == 300 && items[1].rect.width == 300 &&
	      items[2].rect.width == 300);

	check("the outermost column has no fence on its outer side",
	      !tile_resize(&ws, items, 3, 0, TILE_LEFT, 40, area, 0, 0) &&
	      !tile_resize(&ws, items, 3, 2, TILE_RIGHT, 40, area, 0, 0));
	check("and columns do not resize vertically",
	      !tile_resize(&ws, items, 3, 1, TILE_DOWN, 40, area, 0, 0));
}

static void
test_resize_respects_minimums(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);

	reset(3);
	items[2].min_width = 400;
	tile_arrange(&ws, items, 3, area, 0, 0);
	/* Asking for far more than there is room for takes what there is. */
	tile_resize(&ws, items, 3, 1, TILE_RIGHT, 5000, area, 0, 0);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("a window declaring a minimum is not squeezed past it",
	      items[2].rect.width == 400 && items[1].rect.width == 200 &&
	      items[0].rect.width == 300);
	check("and the fence will not move once it is there",
	      !tile_resize(&ws, items, 3, 1, TILE_RIGHT, 50, area, 0, 0));
	check("though it still comes back the other way",
	      tile_resize(&ws, items, 3, 1, TILE_LEFT, -50, area, 0, 0));
}

static void
test_resize_master(void)
{
	struct swc_rectangle area = { 0, 0, 1000, 600 };
	struct tile_ws ws = ws_of(TILE_MASTER);

	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("growing the master rightward widens the master column",
	      tile_resize(&ws, items, 3, 0, TILE_RIGHT, 100, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("by exactly that much",
	      items[0].rect.width == 600 && items[1].rect.x == 600 &&
	      items[1].rect.width == 400);

	check("a stack window growing leftward gives it back",
	      tile_resize(&ws, items, 3, 1, TILE_LEFT, 100, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("and the columns are even again",
	      items[0].rect.width == 500 && items[1].rect.width == 500);

	check("the edges away from the fence do not move",
	      !tile_resize(&ws, items, 3, 0, TILE_LEFT, 50, area, 0, 0) &&
	      !tile_resize(&ws, items, 3, 1, TILE_RIGHT, 50, area, 0, 0));

	check("within the stack, up and down move the fence between its windows",
	      tile_resize(&ws, items, 3, 1, TILE_DOWN, 90, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("and the master column is untouched by it",
	      items[1].rect.height == 390 && items[2].rect.height == 210 &&
	      items[0].rect.height == 600 && items[0].rect.width == 500);

	/* With the master on the right the fence is on its left instead. */
	ws = ws_of(TILE_MASTER);
	ws.master_side = TILE_SIDE_RIGHT;
	reset(3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("with the master on the right the fence is its left edge",
	      tile_resize(&ws, items, 3, 0, TILE_LEFT, 100, area, 0, 0) &&
	      !tile_resize(&ws, items, 3, 0, TILE_RIGHT, 100, area, 0, 0));
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("and it still grows the master",
	      items[0].rect.width == 600 && items[0].rect.x == 400);
}

static void
test_resize_grid(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_GRID);

	reset(4);
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("in a grid, sideways moves the fence inside the row",
	      tile_resize(&ws, items, 4, 0, TILE_RIGHT, 90, area, 0, 0));
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("and leaves the row below alone",
	      items[0].rect.width == 540 && items[1].rect.width == 360 &&
	      items[2].rect.width == 450 && items[3].rect.width == 450);

	check("downward moves the fence between the rows",
	      tile_resize(&ws, items, 4, 0, TILE_DOWN, 60, area, 0, 0));
	tile_arrange(&ws, items, 4, area, 0, 0);
	check("which takes the whole row with it",
	      items[0].rect.height == 360 && items[1].rect.height == 360 &&
	      items[2].rect.height == 240 && items[2].rect.y == 360);

	check("the outer rows have no fence beyond them",
	      !tile_resize(&ws, items, 4, 0, TILE_UP, 40, area, 0, 0) &&
	      !tile_resize(&ws, items, 4, 2, TILE_DOWN, 40, area, 0, 0));
}

static void
test_equalize(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);

	reset(3);
	tile_resize(&ws, items, 3, 0, TILE_RIGHT, 150, area, 0, 0);
	tile_equalize(&ws, items, 3);
	tile_arrange(&ws, items, 3, area, 0, 0);
	check("equalize puts every window back on equal terms",
	      items[0].rect.width == 300 && items[1].rect.width == 300 &&
	      items[2].rect.width == 300 && ws.master_ratio == 0.5);
}

/* ----------------------------------------------------- sizes persist */

static void
test_sizes_survive_a_closing_window(void)
{
	struct swc_rectangle area = { 0, 0, 900, 600 };
	struct tile_ws ws = ws_of(TILE_COLUMNS);
	double kept;

	reset(3);
	tile_resize(&ws, items, 3, 0, TILE_RIGHT, 150, area, 0, 0);
	kept = items[2].main;
	/* The middle window closes: the engine is handed the two that are left,
	 * carrying the weights they had. */
	items[1] = items[2];
	tile_arrange(&ws, items, 2, area, 0, 0);
	check("a window closing does not resize the ones that stay",
	      items[1].main == kept &&
	      items[0].rect.width + items[1].rect.width == 900 &&
	      items[0].rect.width > items[1].rect.width);
}

/* ------------------------------------------------------------- names */

static void
test_names(void)
{
	enum tile_layout layout;
	enum tile_side side;
	bool ok = true;

	for (unsigned i = 0; i < TILE_LAYOUT_LAST; ++i)
		ok = ok && tile_layout_parse(tile_layout_name((enum tile_layout)i),
		                             &layout) && layout == (enum tile_layout)i;
	for (unsigned i = 0; i <= TILE_SIDE_BOTTOM; ++i)
		ok = ok && tile_side_parse(tile_side_name((enum tile_side)i), &side) &&
		     side == (enum tile_side)i;
	check("every layout and side name reads back as itself", ok);
	check("and an unknown one is refused",
	      !tile_layout_parse("spiral", &layout) &&
	      !tile_side_parse("sideways", &side) && !tile_layout_parse(NULL, &layout));
}

/* -------------------------------------------------------------- fuzz */

/*
 * Random work against the invariants. Hand-written cases check the layouts
 * that were thought of; this checks the ones that were not.
 */
static uint32_t seed = 2463534242u;

static uint32_t
rnd(uint32_t bound)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return bound ? seed % bound : 0;
}

static void
test_fuzz(void)
{
	bool ok = true;

	for (unsigned round = 0; round < 4000 && ok; ++round) {
		struct tile_ws ws = ws_of((enum tile_layout)rnd(TILE_LAYOUT_LAST));
		struct swc_rectangle area = { (int32_t)rnd(200) - 100,
		                              (int32_t)rnd(200) - 100,
		                              200 + rnd(1800), 200 + rnd(1000) };
		unsigned n = 1 + rnd(24);
		int32_t inner = (int32_t)rnd(3) * 6, outer = (int32_t)rnd(3) * 7;
		struct swc_rectangle expected = { area.x + outer, area.y + outer,
		                                  area.width - 2 * outer,
		                                  area.height - 2 * outer };

		ws.master_count = 1 + rnd(4);
		ws.master_side = (enum tile_side)rnd(4);
		ws.master_ratio = 0.1 + (double)rnd(80) / 100.0;
		reset(n);
		for (unsigned i = 0; i < n; ++i) {
			items[i].min_width = rnd(4) ? 0 : 50 + rnd(200);
			items[i].min_height = rnd(4) ? 0 : 40 + rnd(150);
		}

		for (unsigned step = 0; step < 12 && ok; ++step) {
			tile_arrange(&ws, items, n, area, inner, outer);

			if (!all_positive(n)) {
				printf("   (empty cell: %s n=%u)\n",
				       tile_layout_name(ws.layout), n);
				ok = false;
				break;
			}
			if (ws.layout != TILE_MONOCLE && !none_overlap(n)) {
				printf("   (overlap: %s n=%u gap=%d)\n",
				       tile_layout_name(ws.layout), n, inner);
				ok = false;
				break;
			}
			for (unsigned i = 0; i < n && ok; ++i) {
				if (!contains(expected, items[i].rect)) {
					printf("   (outside: %s n=%u i=%u)\n",
					       tile_layout_name(ws.layout), n, i);
					ok = false;
				}
			}
			if (ok && n > 1 && ws.layout != TILE_MONOCLE) {
				struct swc_rectangle box = bounding_box(n);

				if (box.x != expected.x || box.y != expected.y ||
				    box.width != expected.width ||
				    box.height != expected.height) {
					printf("   (short of the edges: %s n=%u)\n",
					       tile_layout_name(ws.layout), n);
					ok = false;
				}
			}
			if (!ok)
				break;

			/* Then disturb it and go round again. */
			switch (rnd(4)) {
			case 0:
				tile_resize(&ws, items, n, rnd(n), (enum tile_dir)rnd(4),
				            (int32_t)rnd(400) - 200, area, inner, outer);
				break;
			case 1:
				if (n > 1)
					--n;   /* a window closed */
				break;
			case 2:
				if (n < 24) {
					tile_item_init(&items[n]);
					++n;   /* and another opened */
				}
				break;
			default:
				ws.master_count = 1 + rnd(4);
				break;
			}
		}
	}
	check("four thousand random layouts hold every invariant", ok);
}

int
main(void)
{
	test_exact_without_gaps();
	test_exact_with_uneven_weights();
	test_gaps_stay_inside();
	test_gaps_give_way();
	test_columns_and_rows();
	test_master();
	test_grid();
	test_monocle();
	test_neighbours();
	test_resize_columns();
	test_resize_respects_minimums();
	test_resize_master();
	test_resize_grid();
	test_equalize();
	test_sizes_survive_a_closing_window();
	test_names();
	test_fuzz();
	printf("\n%s\n", failures ? "FAILURES" : "all ok");
	return failures ? 1 : 0;
}
