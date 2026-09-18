#ifndef CHARA_TILING_H
#define CHARA_TILING_H

#include <stdbool.h>
#include <stdint.h>

#include <swc.h>

/*
 * The tiling geometry engine.
 *
 * Everything declared here is pure: it turns a list of windows, an area and a
 * handful of numbers into rectangles, and it never speaks to the compositor,
 * reads a global, or draws anything. src/tiling_layout.c implements it and
 * includes nothing else from charaWC, which is what lets test/tiling_test.c
 * link it on its own and check the geometry without a graphics card.
 *
 * The glue that maps charaWC's windows onto this lives in src/tiling.c, and
 * its prototypes are in config.h with the rest.
 */

enum tile_layout {
	TILE_MASTER,    /* a master area beside a stack of the rest */
	TILE_COLUMNS,   /* every window a full-height column */
	TILE_ROWS,      /* every window a full-width row */
	TILE_GRID,      /* as square as the count allows */
	TILE_MONOCLE,   /* every window fills the area */
	TILE_LAYOUT_LAST
};

/* Which side of the area the master group takes. */
enum tile_side {
	TILE_SIDE_LEFT,
	TILE_SIDE_RIGHT,
	TILE_SIDE_TOP,
	TILE_SIDE_BOTTOM,
};

enum tile_dir { TILE_LEFT, TILE_RIGHT, TILE_UP, TILE_DOWN };

/* Windows a single workspace tiles. Past this the rest are left floating. */
#define TILE_MAX_WINDOWS 256

/*
 * One window, as the engine sees it.
 *
 * `main` and `cross` are the window's share of the group it lands in, and they
 * are the only state a resize writes. They live on the window rather than on
 * the cell so that opening and closing windows cannot scramble the sizes of
 * the ones that stay:
 *
 *   master   main is the height within the master column or the stack column
 *   columns  main is the width
 *   rows     main is the height
 *   grid     main is the width within the row, and cross, on the first window
 *            of each row, is that row's height
 *   monocle  neither is used
 *
 * min_width and min_height are the smallest the *cell* may become, decorations
 * included and gaps excluded. They only ever stop a resize; arranging always
 * produces an exact partition whether they fit or not, because a window that
 * refuses to be small is better than a layout with a hole in it.
 */
struct tile_item {
	double   main, cross;
	uint32_t min_width, min_height;
	struct swc_rectangle rect;   /* written by tile_arrange */
};

/* Per-workspace layout state. */
struct tile_ws {
	enum tile_layout layout;
	enum tile_side   master_side;
	double           master_ratio;   /* 0 < r < 1, master's share */
	unsigned         master_count;   /* at least 1 */
};

void tile_ws_init(struct tile_ws *);
void tile_item_init(struct tile_item *);

/*
 * Fill in every item's rect.
 *
 * `area` is the usable area of the monitor. `outer` is kept clear around the
 * whole of it and `inner` between neighbouring cells; pass zero for either to
 * turn it off. The cells always tile the area exactly -- no gap that was not
 * asked for, no overlap, and nothing lost to rounding.
 */
void tile_arrange(const struct tile_ws *, struct tile_item *, unsigned n,
                  struct swc_rectangle area, int32_t inner, int32_t outer);

/*
 * The sides of `cell` that do not touch the edge of `area`, as
 * SWC_WINDOW_EDGE_* bits. These are the edges the window is tiled against,
 * which is what a client needs to know to stop rounding those corners.
 */
uint32_t tile_edges(struct swc_rectangle cell, struct swc_rectangle area);

/*
 * The window neighbouring `from` in `dir`, or -1 when there is none.
 *
 * Purely geometric, so it reads the same in every layout: among the windows on
 * that side, the one sharing the most edge with `from`, and the nearest of
 * those. Requires that tile_arrange has run.
 */
int tile_neighbour(const struct tile_item *, unsigned n, unsigned from,
                   enum tile_dir);

/*
 * The window whose cell contains the point, or -1. tile_arrange must have run.
 */
int tile_at(const struct tile_item *, unsigned n, int32_t x, int32_t y);

/*
 * Move window i's `dir` edge outward by `delta` pixels, or inward when delta
 * is negative, by moving the fence it shares with its neighbour there.
 *
 * Writes weights, or the master ratio, and leaves the rects alone: arrange
 * again to see the result. Returns false when there is no fence on that side,
 * or when moving it would push a window below its minimum. The area and gaps
 * must be the ones the layout was last arranged with.
 */
bool tile_resize(struct tile_ws *, struct tile_item *, unsigned n, unsigned i,
                 enum tile_dir, int32_t delta, struct swc_rectangle area,
                 int32_t inner, int32_t outer);

/* Every window back to an equal share, and the master ratio back to its half. */
void tile_equalize(struct tile_ws *, struct tile_item *, unsigned n);

const char *tile_layout_name(enum tile_layout);
bool tile_layout_parse(const char *, enum tile_layout *);
const char *tile_side_name(enum tile_side);
bool tile_side_parse(const char *, enum tile_side *);
const char *tile_dir_name(enum tile_dir);

#endif /* CHARA_TILING_H */
