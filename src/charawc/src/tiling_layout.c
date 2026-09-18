/*
 * Tiling geometry.
 *
 * This file is pure. It includes swc.h for struct swc_rectangle and the
 * SWC_WINDOW_EDGE_* names and calls nothing from it; it reads no global and
 * touches no window. Everything here is a function from numbers to numbers,
 * which is what lets test/tiling_test.c link it on its own.
 *
 * The one invariant worth stating up front: after tile_arrange, the cells tile
 * the area exactly. Adjacent cells are separated by the inner gap and nothing
 * else -- no rounding slack, no overlap -- at every level of every layout.
 * Everything below is arranged to make that true rather than nearly true.
 */
#include <math.h>
#include <string.h>

#include "tiling.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ------------------------------------------------------------- names */

static const char *const layout_names[TILE_LAYOUT_LAST] = {
	[TILE_MASTER]  = "master",
	[TILE_COLUMNS] = "columns",
	[TILE_ROWS]    = "rows",
	[TILE_GRID]    = "grid",
	[TILE_MONOCLE] = "monocle",
};

static const char *const side_names[] = {
	[TILE_SIDE_LEFT]   = "left",
	[TILE_SIDE_RIGHT]  = "right",
	[TILE_SIDE_TOP]    = "top",
	[TILE_SIDE_BOTTOM] = "bottom",
};

static const char *const dir_names[] = {
	[TILE_LEFT]  = "left",
	[TILE_RIGHT] = "right",
	[TILE_UP]    = "up",
	[TILE_DOWN]  = "down",
};

const char *
tile_layout_name(enum tile_layout layout)
{
	return layout < TILE_LAYOUT_LAST ? layout_names[layout] : "?";
}

bool
tile_layout_parse(const char *name, enum tile_layout *out)
{
	if (!name)
		return false;
	for (unsigned i = 0; i < TILE_LAYOUT_LAST; ++i) {
		if (!strcmp(name, layout_names[i])) {
			*out = (enum tile_layout)i;
			return true;
		}
	}
	return false;
}

const char *
tile_side_name(enum tile_side side)
{
	return side <= TILE_SIDE_BOTTOM ? side_names[side] : "?";
}

bool
tile_side_parse(const char *name, enum tile_side *out)
{
	if (!name)
		return false;
	for (unsigned i = 0; i <= TILE_SIDE_BOTTOM; ++i) {
		if (!strcmp(name, side_names[i])) {
			*out = (enum tile_side)i;
			return true;
		}
	}
	return false;
}

const char *
tile_dir_name(enum tile_dir dir)
{
	return dir <= TILE_DOWN ? dir_names[dir] : "?";
}

void
tile_ws_init(struct tile_ws *ws)
{
	ws->layout = TILE_MASTER;
	ws->master_side = TILE_SIDE_LEFT;
	ws->master_ratio = 0.5;
	ws->master_count = 1;
}

void
tile_item_init(struct tile_item *item)
{
	memset(item, 0, sizeof(*item));
	item->main = 1.0;
	item->cross = 1.0;
}

/* ------------------------------------------------------- directions */

static bool
dir_horizontal(enum tile_dir dir)
{
	return dir == TILE_LEFT || dir == TILE_RIGHT;
}

/* Left and up run toward the origin; right and down away from it. */
static bool
dir_forward(enum tile_dir dir)
{
	return dir == TILE_RIGHT || dir == TILE_DOWN;
}

static bool
side_horizontal(enum tile_side side)
{
	return side == TILE_SIDE_LEFT || side == TILE_SIDE_RIGHT;
}

/* Whether the master group sits at the low end of its axis. */
static bool
side_first(enum tile_side side)
{
	return side == TILE_SIDE_LEFT || side == TILE_SIDE_TOP;
}

/* --------------------------------------------------------- rectangles */

static struct swc_rectangle
shrink(struct swc_rectangle r, int32_t by)
{
	int32_t width, height;

	if (by <= 0)
		return r;
	width = (int32_t)r.width - 2 * by;
	height = (int32_t)r.height - 2 * by;
	r.x += by;
	r.y += by;
	r.width = width > 1 ? (uint32_t)width : 1;
	r.height = height > 1 ? (uint32_t)height : 1;
	return r;
}

static int64_t
span_overlap(int32_t a, uint32_t alen, int32_t b, uint32_t blen)
{
	int64_t low = MAX((int64_t)a, (int64_t)b);
	int64_t high = MIN((int64_t)a + alen, (int64_t)b + blen);
	return high > low ? high - low : 0;
}

uint32_t
tile_edges(struct swc_rectangle cell, struct swc_rectangle area)
{
	uint32_t edges = 0;

	if (cell.x > area.x)
		edges |= SWC_WINDOW_EDGE_LEFT;
	if (cell.y > area.y)
		edges |= SWC_WINDOW_EDGE_TOP;
	if (cell.x + (int32_t)cell.width < area.x + (int32_t)area.width)
		edges |= SWC_WINDOW_EDGE_RIGHT;
	if (cell.y + (int32_t)cell.height < area.y + (int32_t)area.height)
		edges |= SWC_WINDOW_EDGE_BOTTOM;
	return edges;
}

/* ---------------------------------------------------------- splitting */

/*
 * The gap actually used between n parts of `extent`.
 *
 * Gaps are a decoration and windows are not, so when the two cannot both fit
 * the gap is what gives way. Without this a workspace narrower than
 * (n - 1) * gap would produce cells of no width at all.
 */
static int32_t
usable_gap(int32_t extent, unsigned n, int32_t gap)
{
	int32_t room;

	if (gap < 0)
		gap = 0;
	if (n < 2)
		return 0;
	if (extent - (int32_t)(n - 1) * gap >= (int32_t)n)
		return gap;
	room = (extent - (int32_t)n) / (int32_t)(n - 1);
	return room > 0 ? room : 0;
}

/* The pixels n parts share once the gaps between them are taken out. */
static int32_t
net_extent(int32_t extent, unsigned n, int32_t gap)
{
	int32_t net;

	if (!n)
		return 0;
	net = extent - (int32_t)(n - 1) * usable_gap(extent, n, gap);
	return net < (int32_t)n ? (int32_t)n : net;
}

/*
 * Split `extent` into n parts in proportion to w, separated by `gap`.
 *
 * Each part's far boundary is computed from the running weight total against
 * the full extent, never by adding up the sizes before it, so the rounding
 * error cannot accumulate: the parts and the gaps add up to `extent` exactly.
 * The boundaries are then squeezed into [previous + 1, avail - (left to come)],
 * which keeps every part at least one pixel wide without breaking that.
 */
static void
distribute(int32_t extent, const double *w, unsigned n, int32_t gap,
           int32_t *offset, int32_t *size)
{
	int32_t used_gap = usable_gap(extent, n, gap);
	int32_t avail = net_extent(extent, n, gap);
	int32_t previous = 0;
	double total = 0, running = 0;

	if (!n)
		return;
	for (unsigned i = 0; i < n; ++i)
		total += w[i] > 0 ? w[i] : 0;

	for (unsigned i = 0; i < n; ++i) {
		int32_t boundary, low, high;

		running += total > 0 ? (w[i] > 0 ? w[i] : 0) : 1;
		if (i + 1 == n)
			boundary = avail;
		else
			boundary = (int32_t)llround((double)avail * running /
			                            (total > 0 ? total : (double)n));
		low = previous + 1;
		high = avail - (int32_t)(n - 1 - i);
		if (boundary > high)
			boundary = high;
		if (boundary < low)
			boundary = low;
		offset[i] = previous + (int32_t)i * used_gap;
		size[i] = boundary - previous;
		previous = boundary;
	}
}

/*
 * Lay `count` windows out along one axis of `box`, in the order given.
 *
 * `cross` picks which weight to read: the grid divides its rows by `cross` and
 * everything else divides by `main`.
 */
static void
lay_out(struct tile_item *items, const unsigned *index, unsigned count,
        struct swc_rectangle box, bool horizontal, int32_t gap, bool cross)
{
	double w[TILE_MAX_WINDOWS];
	int32_t offset[TILE_MAX_WINDOWS], size[TILE_MAX_WINDOWS];

	if (!count)
		return;
	for (unsigned i = 0; i < count; ++i)
		w[i] = cross ? items[index[i]].cross : items[index[i]].main;
	distribute(horizontal ? (int32_t)box.width : (int32_t)box.height,
	           w, count, gap, offset, size);
	for (unsigned i = 0; i < count; ++i) {
		struct swc_rectangle *r = &items[index[i]].rect;

		*r = box;
		if (horizontal) {
			r->x = box.x + offset[i];
			r->width = (uint32_t)size[i];
		} else {
			r->y = box.y + offset[i];
			r->height = (uint32_t)size[i];
		}
	}
}

/* items[first .. first + count - 1], as lay_out wants them. */
static void
lay_out_range(struct tile_item *items, unsigned first, unsigned count,
              struct swc_rectangle box, bool horizontal, int32_t gap)
{
	unsigned index[TILE_MAX_WINDOWS];

	for (unsigned i = 0; i < count; ++i)
		index[i] = first + i;
	lay_out(items, index, count, box, horizontal, gap, false);
}

/* -------------------------------------------------------------- grid */

/*
 * How many rows n windows make.
 *
 * Columns are taken first -- ceil(sqrt(n)) of them -- so the grid grows wider
 * before it grows taller, which suits a monitor that is wider than it is tall.
 */
static unsigned
grid_rows(unsigned n)
{
	unsigned cols = 1, rows;

	while (cols * cols < n)
		++cols;
	rows = (n + cols - 1) / cols;
	return rows ? rows : 1;
}

/* Where row r starts and how many windows it holds. Earlier rows take the
 * remainder, so a partly filled grid is heavy at the top. */
static void
grid_row(unsigned n, unsigned rows, unsigned r, unsigned *first, unsigned *count)
{
	unsigned base = n / rows, extra = n % rows;

	*first = r * base + (r < extra ? r : extra);
	*count = base + (r < extra ? 1 : 0);
}

static unsigned
grid_row_of(unsigned n, unsigned rows, unsigned i)
{
	for (unsigned r = 0; r < rows; ++r) {
		unsigned first, count;

		grid_row(n, rows, r, &first, &count);
		if (i >= first && i < first + count)
			return r;
	}
	return 0;
}

/* The height each row is given, before the windows in it are placed. */
static void
grid_rows_geometry(struct tile_item *items, unsigned n, unsigned rows,
                   int32_t extent, int32_t gap, int32_t *offset, int32_t *size)
{
	double w[TILE_MAX_WINDOWS];

	for (unsigned r = 0; r < rows; ++r) {
		unsigned first, count;

		grid_row(n, rows, r, &first, &count);
		w[r] = count ? items[first].cross : 1.0;
	}
	distribute(extent, w, rows, gap, offset, size);
}

/* ------------------------------------------------------------ master */

/* How many of the n windows the master group holds. */
static unsigned
master_group(const struct tile_ws *ws, unsigned n)
{
	unsigned nm = ws->master_count ? ws->master_count : 1;
	return nm > n ? n : nm;
}

/*
 * The two group sizes along the master axis, and the gap between them.
 *
 * Returns false when there is no fence to speak of -- a workspace whose
 * windows all fit in the master group is laid out as a single run.
 */
static bool
master_split(const struct tile_ws *ws, unsigned n, int32_t extent,
             int32_t gap, int32_t *master_px, int32_t *stack_px, int32_t *used_gap)
{
	int32_t avail;
	double ratio = ws->master_ratio;

	if (master_group(ws, n) == n)
		return false;
	*used_gap = usable_gap(extent, 2, gap);
	avail = net_extent(extent, 2, gap);
	if (ratio <= 0 || ratio >= 1 || !(ratio == ratio))
		ratio = 0.5;
	*master_px = (int32_t)llround((double)avail * ratio);
	if (*master_px < 1)
		*master_px = 1;
	if (*master_px > avail - 1)
		*master_px = avail - 1;
	*stack_px = avail - *master_px;
	return true;
}

/* The smallest the group may be along the master axis: every window in it
 * spans that axis, so the group is as wide as its widest member needs. */
static int32_t
group_minimum(const struct tile_item *items, unsigned first, unsigned count,
              bool horizontal)
{
	int32_t least = 1;

	for (unsigned i = first; i < first + count; ++i) {
		int32_t want = horizontal ? (int32_t)items[i].min_width
		                          : (int32_t)items[i].min_height;
		if (want > least)
			least = want;
	}
	return least;
}

static void
arrange_master(const struct tile_ws *ws, struct tile_item *items, unsigned n,
               struct swc_rectangle area, int32_t inner)
{
	bool horizontal = side_horizontal(ws->master_side);
	bool first = side_first(ws->master_side);
	unsigned nm = master_group(ws, n);
	int32_t extent = horizontal ? (int32_t)area.width : (int32_t)area.height;
	int32_t master_px, stack_px, gap;
	struct swc_rectangle master_box = area, stack_box = area;

	if (!master_split(ws, n, extent, inner, &master_px, &stack_px, &gap)) {
		/* Nothing in the stack, so the master group takes the lot. Windows
		 * still stack across the master axis, which is what makes going from
		 * one window to two a split rather than a rearrangement. */
		lay_out_range(items, 0, n, area, !horizontal, inner);
		return;
	}

	if (horizontal) {
		master_box.width = (uint32_t)master_px;
		stack_box.width = (uint32_t)stack_px;
		if (first)
			stack_box.x = area.x + master_px + gap;
		else
			master_box.x = area.x + stack_px + gap;
	} else {
		master_box.height = (uint32_t)master_px;
		stack_box.height = (uint32_t)stack_px;
		if (first)
			stack_box.y = area.y + master_px + gap;
		else
			master_box.y = area.y + stack_px + gap;
	}
	lay_out_range(items, 0, nm, master_box, !horizontal, inner);
	lay_out_range(items, nm, n - nm, stack_box, !horizontal, inner);
}

/* ----------------------------------------------------------- arrange */

void
tile_arrange(const struct tile_ws *ws, struct tile_item *items, unsigned n,
             struct swc_rectangle area, int32_t inner, int32_t outer)
{
	struct swc_rectangle inner_area;

	if (!n)
		return;
	if (n > TILE_MAX_WINDOWS)
		n = TILE_MAX_WINDOWS;
	inner_area = shrink(area, outer);

	switch (ws->layout) {
	case TILE_MONOCLE:
		for (unsigned i = 0; i < n; ++i)
			items[i].rect = inner_area;
		break;
	case TILE_COLUMNS:
		lay_out_range(items, 0, n, inner_area, true, inner);
		break;
	case TILE_ROWS:
		lay_out_range(items, 0, n, inner_area, false, inner);
		break;
	case TILE_GRID: {
		unsigned rows = grid_rows(n);
		int32_t offset[TILE_MAX_WINDOWS], size[TILE_MAX_WINDOWS];

		grid_rows_geometry(items, n, rows, (int32_t)inner_area.height, inner,
		                   offset, size);
		for (unsigned r = 0; r < rows; ++r) {
			unsigned first, count;
			struct swc_rectangle box = inner_area;

			grid_row(n, rows, r, &first, &count);
			box.y = inner_area.y + offset[r];
			box.height = (uint32_t)size[r];
			lay_out_range(items, first, count, box, true, inner);
		}
		break;
	}
	case TILE_MASTER:
	default:
		arrange_master(ws, items, n, inner_area, inner);
		break;
	}
}

/* -------------------------------------------------------- neighbours */

int
tile_neighbour(const struct tile_item *items, unsigned n, unsigned from,
               enum tile_dir dir)
{
	const struct swc_rectangle *a;
	int best = -1;
	int64_t best_distance = 0, best_overlap = 0, best_centre = 0;

	if (from >= n)
		return -1;
	a = &items[from].rect;

	for (unsigned j = 0; j < n; ++j) {
		const struct swc_rectangle *b = &items[j].rect;
		int64_t distance, overlap, centre;
		bool better;

		if (j == from)
			continue;
		switch (dir) {
		case TILE_LEFT:
			distance = (int64_t)a->x - ((int64_t)b->x + b->width);
			overlap = span_overlap(a->y, a->height, b->y, b->height);
			centre = ((int64_t)a->y * 2 + a->height) -
			         ((int64_t)b->y * 2 + b->height);
			break;
		case TILE_RIGHT:
			distance = (int64_t)b->x - ((int64_t)a->x + a->width);
			overlap = span_overlap(a->y, a->height, b->y, b->height);
			centre = ((int64_t)a->y * 2 + a->height) -
			         ((int64_t)b->y * 2 + b->height);
			break;
		case TILE_UP:
			distance = (int64_t)a->y - ((int64_t)b->y + b->height);
			overlap = span_overlap(a->x, a->width, b->x, b->width);
			centre = ((int64_t)a->x * 2 + a->width) -
			         ((int64_t)b->x * 2 + b->width);
			break;
		default:
			distance = (int64_t)b->y - ((int64_t)a->y + a->height);
			overlap = span_overlap(a->x, a->width, b->x, b->width);
			centre = ((int64_t)a->x * 2 + a->width) -
			         ((int64_t)b->x * 2 + b->width);
			break;
		}
		/* Behind, or on top of, rather than beside: not a neighbour that
		 * way. This is also what leaves monocle without any. */
		if (distance < 0)
			continue;
		if (centre < 0)
			centre = -centre;

		if (best < 0) {
			better = true;
		} else if ((best_overlap > 0) != (overlap > 0)) {
			/* Sharing an edge beats merely being over there. */
			better = overlap > 0;
		} else if (distance != best_distance) {
			better = distance < best_distance;
		} else if (overlap != best_overlap) {
			better = overlap > best_overlap;
		} else {
			better = centre < best_centre;
		}
		if (better) {
			best = (int)j;
			best_distance = distance;
			best_overlap = overlap;
			best_centre = centre;
		}
	}
	return best;
}

int
tile_at(const struct tile_item *items, unsigned n, int32_t x, int32_t y)
{
	for (unsigned i = 0; i < n; ++i) {
		const struct swc_rectangle *r = &items[i].rect;

		if (x >= r->x && y >= r->y && x < r->x + (int32_t)r->width &&
		    y < r->y + (int32_t)r->height)
			return (int)i;
	}
	return -1;
}

/* ------------------------------------------------------------ resize */

/*
 * Move the fence between two windows of one run by `delta` pixels: a grows and
 * b shrinks when delta is positive.
 *
 * The weight is moved from one to the other rather than scaled, so the run's
 * total is unchanged and every other window in it keeps the size it had.
 * `net` is the run's pixels once its gaps are taken out, which is the exchange
 * rate between a pixel and a unit of weight.
 */
static bool
transfer(double *wa, double *wb, int32_t min_a, int32_t min_b,
         double total, int32_t net, int32_t delta)
{
	double dw, low, high, rate;

	if (net <= 0 || total <= 0 || delta == 0)
		return false;
	rate = total / (double)net;
	dw = (double)delta * rate;
	/* The furthest the fence may travel before one side is too small. */
	low = (double)MAX(min_a, 1) * rate - *wa;
	high = *wb - (double)MAX(min_b, 1) * rate;
	if (low > high)
		return false;
	if (dw < low)
		dw = low;
	if (dw > high)
		dw = high;
	if (dw > -1e-9 && dw < 1e-9)
		return false;
	*wa += dw;
	*wb -= dw;
	return true;
}

/* The weight a run holds, and the pixels it has to share out. */
static double
run_total(const struct tile_item *items, unsigned first, unsigned count)
{
	double total = 0;

	for (unsigned i = first; i < first + count; ++i)
		total += items[i].main > 0 ? items[i].main : 0;
	return total > 0 ? total : (double)count;
}

/* Resize within one run of consecutive windows laid out along `horizontal`. */
static bool
resize_in_run(struct tile_item *items, unsigned first, unsigned count,
              unsigned i, enum tile_dir dir, int32_t delta, int32_t extent,
              int32_t gap)
{
	unsigned j;
	int32_t min_a, min_b;
	bool horizontal = dir_horizontal(dir);

	if (count < 2)
		return false;
	if (dir_forward(dir)) {
		if (i + 1 >= first + count)
			return false;
		j = i + 1;
	} else {
		if (i <= first)
			return false;
		j = i - 1;
	}
	min_a = horizontal ? (int32_t)items[i].min_width : (int32_t)items[i].min_height;
	min_b = horizontal ? (int32_t)items[j].min_width : (int32_t)items[j].min_height;
	return transfer(&items[i].main, &items[j].main, min_a, min_b,
	                run_total(items, first, count),
	                net_extent(extent, count, gap), delta);
}

static bool
resize_grid(struct tile_item *items, unsigned n, unsigned i, enum tile_dir dir,
            int32_t delta, struct swc_rectangle area, int32_t inner)
{
	unsigned rows, row, first, count;

	rows = grid_rows(n);
	row = grid_row_of(n, rows, i);
	grid_row(n, rows, row, &first, &count);

	if (dir_horizontal(dir))
		return resize_in_run(items, first, count, i, dir, delta,
		                     (int32_t)area.width, inner);

	/* Vertically the fence is between two whole rows, and a row's height is
	 * kept on its first window. */
	{
		unsigned other, other_first, other_count;
		double total = 0;
		int32_t min_a = 1, min_b = 1;

		if (rows < 2)
			return false;
		if (dir_forward(dir)) {
			if (row + 1 >= rows)
				return false;
			other = row + 1;
		} else {
			if (row == 0)
				return false;
			other = row - 1;
		}
		grid_row(n, rows, other, &other_first, &other_count);
		for (unsigned r = 0; r < rows; ++r) {
			unsigned f, c;

			grid_row(n, rows, r, &f, &c);
			total += (c && items[f].cross > 0) ? items[f].cross : 1.0;
		}
		min_a = group_minimum(items, first, count, false);
		min_b = group_minimum(items, other_first, other_count, false);
		return transfer(&items[first].cross, &items[other_first].cross,
		                min_a, min_b, total,
		                net_extent((int32_t)area.height, rows, inner), delta);
	}
}

static bool
resize_master(struct tile_ws *ws, struct tile_item *items, unsigned n,
              unsigned i, enum tile_dir dir, int32_t delta,
              struct swc_rectangle area, int32_t inner)
{
	bool horizontal = side_horizontal(ws->master_side);
	bool first = side_first(ws->master_side);
	unsigned nm = master_group(ws, n);
	bool in_master = i < nm;
	int32_t extent = horizontal ? (int32_t)area.width : (int32_t)area.height;
	int32_t cross = horizontal ? (int32_t)area.height : (int32_t)area.width;
	int32_t master_px, stack_px, gap, avail, want, least_master, least_stack;
	bool toward_fence;

	if (dir_horizontal(dir) != horizontal) {
		/* Across the master axis: an ordinary fence inside one group. */
		if (in_master)
			return resize_in_run(items, 0, nm, i, dir, delta, cross, inner);
		return resize_in_run(items, nm, n - nm, i, dir, delta, cross, inner);
	}
	if (!master_split(ws, n, extent, inner, &master_px, &stack_px, &gap))
		return false;
	/*
	 * Along the master axis there is exactly one fence, between the two
	 * groups, and only the edge that touches it can move -- a group's other
	 * edge is the edge of the workspace. Which edge that is depends on which
	 * side the master group is on: with the master first the fence is the
	 * master's far edge and the stack's near one, and the other way round
	 * when it is last.
	 */
	toward_fence = in_master ? dir_forward(dir) == first
	                         : dir_forward(dir) != first;
	if (!toward_fence)
		return false;
	/* Growing the master group moves the fence out; growing a stack window
	 * against the same fence moves it the other way. */
	want = in_master ? master_px + delta : master_px - delta;

	avail = master_px + stack_px;
	least_master = group_minimum(items, 0, nm, horizontal);
	least_stack = group_minimum(items, nm, n - nm, horizontal);
	if (least_master + least_stack > avail)
		return false;
	if (want < least_master)
		want = least_master;
	if (want > avail - least_stack)
		want = avail - least_stack;
	if (want == master_px)
		return false;
	ws->master_ratio = (double)want / (double)avail;
	return true;
}

bool
tile_resize(struct tile_ws *ws, struct tile_item *items, unsigned n, unsigned i,
            enum tile_dir dir, int32_t delta, struct swc_rectangle area,
            int32_t inner, int32_t outer)
{
	struct swc_rectangle inner_area;

	if (!n || i >= n || n > TILE_MAX_WINDOWS || delta == 0)
		return false;
	inner_area = shrink(area, outer);

	switch (ws->layout) {
	case TILE_MONOCLE:
		return false;
	case TILE_COLUMNS:
		if (!dir_horizontal(dir))
			return false;
		return resize_in_run(items, 0, n, i, dir, delta,
		                     (int32_t)inner_area.width, inner);
	case TILE_ROWS:
		if (dir_horizontal(dir))
			return false;
		return resize_in_run(items, 0, n, i, dir, delta,
		                     (int32_t)inner_area.height, inner);
	case TILE_GRID:
		return resize_grid(items, n, i, dir, delta, inner_area, inner);
	case TILE_MASTER:
	default:
		return resize_master(ws, items, n, i, dir, delta, inner_area, inner);
	}
}

void
tile_equalize(struct tile_ws *ws, struct tile_item *items, unsigned n)
{
	for (unsigned i = 0; i < n; ++i) {
		items[i].main = 1.0;
		items[i].cross = 1.0;
	}
	ws->master_ratio = 0.5;
}
