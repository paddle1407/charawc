/*
 * Tiling: the part that talks to the compositor.
 *
 * The geometry itself is in tiling_layout.c and knows nothing about charaWC.
 * This file owns the other half: which windows belong to which workspace's
 * layout and in what order, when to lay one out again, and how the resulting
 * rectangles reach swc.
 *
 * Two things are worth knowing before reading it.
 *
 * A window's membership is c->tiled, and it survives everything. A window that
 * goes fullscreen, is minimized, or is maximized stays a member: it is only
 * left out of the arrangement while it is in that state, and it comes back to
 * the place it had. Only the user taking it out of the tiling, or the window
 * closing, ends the membership.
 *
 * Laying out is deferred. Anything that changes a workspace marks it and
 * returns; the work happens once, in an idle callback at the end of the event
 * loop turn. swc coalesces configure events over the same turn, so a burst of
 * a dozen events costs one configure per window rather than a dozen.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define MAX_SCREENS 16

static struct wl_event_loop *event_loop;
static bool arrange_queued;

/*
 * Arranging slides windows about under a pointer that has not moved, and the
 * enter events that follow are not the user pointing at anything. Remember
 * where the pointer was when the layout last moved something; an enter from
 * that same spot is the layout's doing and not a reason to change focus.
 */
static bool settling;
static int32_t settled_x, settled_y;

/* One workspace's layout, worked out but not yet applied. */
struct plan {
	struct client   *clients[TILE_MAX_WINDOWS];
	struct tile_item items[TILE_MAX_WINDOWS];
	unsigned         n;
	struct swc_rectangle area;
	int32_t          inner, outer;
};

/* ------------------------------------------------------------ members */

/* Whether a member is in the arrangement at this moment. A window that is
 * fullscreen, minimized or maximized keeps its membership and its place; it
 * simply is not on the grid while it is like that. */
static bool
laid_out(const struct client *c)
{
	return c->tiled && !c->minimized && !c->fullscreen && !c->maximized;
}

/*
 * Every member of a workspace's tiling, in layout order.
 *
 * `showing` leaves out the ones that are not on the grid right now. The full
 * list is what the ordering is kept in: a minimized window has to hold its
 * place, so the order has to stay consistent whether it is showing or not.
 */
static unsigned
collect(const struct screen *s, uint8_t ws, bool showing, struct client **out,
        unsigned max)
{
	struct client *c;
	unsigned n = 0;

	if (!s)
		return 0;
	wl_list_for_each(c, &wm.clients, link) {
		unsigned at;

		if (!c->tiled || c->scr != s || c->ws != ws)
			continue;
		if (showing && !laid_out(c))
			continue;
		if (n == max)
			break;
		/* Insertion sort by order: it is the layout's order that matters,
		 * not the creation order the list is kept in, and a workspace never
		 * holds enough windows for this to be worth anything cleverer. */
		for (at = n; at > 0 && out[at - 1]->tile_order > c->tile_order; --at)
			out[at] = out[at - 1];
		out[at] = c;
		++n;
	}
	return n;
}

/* Close up the gaps the orders leave behind, so they stay 0 to n-1. */
static void
renumber(struct screen *s, uint8_t ws)
{
	struct client *members[TILE_MAX_WINDOWS];
	unsigned n = collect(s, ws, false, members, TILE_MAX_WINDOWS);

	for (unsigned i = 0; i < n; ++i)
		members[i]->tile_order = i;
}

/* Where a window joining the tiling lands, and the shuffle that makes room. */
static void
place_in_order(struct client *c)
{
	struct client *members[TILE_MAX_WINDOWS];
	unsigned n, at, kept = 0;

	c->tile_order = 0;
	n = collect(c->scr, c->ws, false, members, TILE_MAX_WINDOWS);
	/* The window being placed is a member already, and it cannot be one of
	 * the windows it is being fitted between. */
	for (unsigned i = 0; i < n; ++i)
		if (members[i] != c)
			members[kept++] = members[i];
	n = kept;
	if (!n)
		return;

	switch (config.tiling.insert) {
	case TILE_INSERT_START:
		at = 0;
		break;
	case TILE_INSERT_END:
		at = n;
		break;
	case TILE_INSERT_AFTER_FOCUS:
	default:
		at = n;
		if (wm.cur && wm.cur != c && wm.cur->tiled && wm.cur->scr == c->scr &&
		    wm.cur->ws == c->ws)
			at = wm.cur->tile_order + 1;
		break;
	}
	if (at > n)
		at = n;
	for (unsigned i = 0; i < n; ++i)
		members[i]->tile_order = i < at ? i : i + 1;
	c->tile_order = at;
}

/*
 * Two windows exchange cells.
 *
 * Their places in the order and the sizes that belong to those places both
 * change hands, so the layout comes out identical and only the windows filling
 * it have moved. Carrying the weights along is what stops a swap resizing
 * things behind the user's back.
 */
static void
swap_places(struct client *a, struct client *b)
{
	unsigned order = a->tile_order;
	double main = a->tile_main, cross = a->tile_cross;

	a->tile_order = b->tile_order;
	a->tile_main = b->tile_main;
	a->tile_cross = b->tile_cross;
	b->tile_order = order;
	b->tile_main = main;
	b->tile_cross = cross;
}

/* ------------------------------------------------------------ the plan */

/*
 * The smallest a window's cell may be.
 *
 * The client asks about its content; the cell also has to hold the frame the
 * window manager draws around it. This only ever stops a resize -- arranging
 * always produces an exact partition whether the minimums fit in it or not,
 * because a window that refuses to be small is better than a hole in the
 * layout.
 */
static void
minimum(const struct client *c, uint32_t *width, uint32_t *height)
{
	int32_t side = chara_border_width();
	int32_t top = side + chara_titlebar_height(c);
	uint32_t want_width = c->win && c->win->min_width ? c->win->min_width : 1;
	uint32_t want_height = c->win && c->win->min_height ? c->win->min_height : 1;

	*width = want_width + (uint32_t)(2 * side);
	*height = want_height + (uint32_t)(top + side);
}

/* Work out where everything on a workspace goes, without moving anything. */
static bool
build_plan(struct screen *s, uint8_t ws, struct plan *p)
{
	memset(p, 0, sizeof(*p));
	if (!s || !s->scr || ws < 1 || ws > CHARA_WORKSPACES)
		return false;
	p->n = collect(s, ws, true, p->clients, TILE_MAX_WINDOWS);
	if (!p->n)
		return false;

	for (unsigned i = 0; i < p->n; ++i) {
		struct tile_item *item = &p->items[i];

		tile_item_init(item);
		item->main = p->clients[i]->tile_main;
		item->cross = p->clients[i]->tile_cross;
		minimum(p->clients[i], &item->min_width, &item->min_height);
	}
	p->area = s->scr->usable_geometry;
	p->inner = config.tiling.inner_gap;
	p->outer = config.tiling.outer_gap;
	if (config.tiling.smart_gaps && p->n == 1)
		p->inner = p->outer = 0;
	tile_arrange(&s->tiles[ws], p->items, p->n, p->area, p->inner, p->outer);
	return true;
}

static int
index_of(const struct plan *p, const struct client *c)
{
	for (unsigned i = 0; i < p->n; ++i)
		if (p->clients[i] == c)
			return (int)i;
	return -1;
}

static void
write_back(const struct plan *p)
{
	for (unsigned i = 0; i < p->n; ++i) {
		p->clients[i]->tile_main = p->items[i].main;
		p->clients[i]->tile_cross = p->items[i].cross;
	}
}

/* ---------------------------------------------------------- applying */

static bool
apply(struct client *c, struct swc_rectangle cell, struct swc_rectangle area)
{
	struct swc_rectangle content = chara_frame_inset(c, cell);
	uint32_t edges = tile_edges(cell, area);
	bool moved = content.x != c->x || content.y != c->y ||
	             content.width != c->width || content.height != c->height;

	/*
	 * Saying the mode again is not free: swc reissues the configure and
	 * drops the acknowledgement that a pending move is waiting on, which
	 * would undo the very thing that keeps a resize from tearing. Only say
	 * it when it is news.
	 */
	if (!c->tile_mode_set) {
		swc_window_set_tiled(c->win);
		c->tile_mode_set = true;
		c->tile_edges = ~0u;   /* whatever was published went with it */
	}
	if (edges != c->tile_edges) {
		swc_window_set_tiled_edges(c->win, edges);
		c->tile_edges = edges;
	}
	swc_window_set_geometry(c->win, &content);
	c->x = content.x;
	c->y = content.y;
	c->width = content.width;
	c->height = content.height;
	return moved;
}

static void
arrange(struct screen *s, uint8_t ws)
{
	struct plan p;
	bool was_settling = settling, moved = false;
	int32_t was_x = settled_x, was_y = settled_y;

	if (!build_plan(s, ws, &p))
		return;
	/* Noted before applying rather than after: moving a view can make the
	 * pointer enter a window then and there, and that enter is the one this
	 * is here to catch. */
	settling = swc_cursor_position(&settled_x, &settled_y);
	for (unsigned i = 0; i < p.n; ++i)
		moved |= apply(p.clients[i], p.items[i].rect, p.area);
	/* Monocle hands every window the same rectangle, one on top of another,
	 * so which one is on top is the whole of what you can see. It has to be
	 * the focused one, or switching to monocle shows whatever happened to be
	 * raised last. */
	if (s->tiles[ws].layout == TILE_MONOCLE && wm.cur && wm.cur->scr == s &&
	    wm.cur->ws == ws && laid_out(wm.cur))
		swc_window_raise(wm.cur->win);
	if (!moved) {
		settling = was_settling;
		settled_x = was_x;
		settled_y = was_y;
	}
}

static void
drain(void *data)
{
	struct screen *s;

	(void)data;
	arrange_queued = false;
	wl_list_for_each(s, &wm.screens, link) {
		for (uint8_t ws = 1; ws <= CHARA_WORKSPACES; ++ws) {
			if (!s->tile_dirty[ws])
				continue;
			s->tile_dirty[ws] = false;
			/*
			 * Workspaces that are not on screen are laid out too. Their
			 * windows are hidden, so it costs nothing to look at, and it
			 * means they have the right buffer in hand before they are
			 * shown rather than a frame afterwards.
			 */
			arrange(s, ws);
		}
	}
}

void
chara_tiling_dirty(struct screen *s, uint8_t ws)
{
	if (!s || ws < 1 || ws > CHARA_WORKSPACES)
		return;
	s->tile_dirty[ws] = true;
	if (arrange_queued || !event_loop)
		return;
	arrange_queued = wl_event_loop_add_idle(event_loop, drain, NULL) != NULL;
	/* Out of memory for an idle source: do the work now rather than never. */
	if (!arrange_queued)
		drain(NULL);
}

void
chara_tiling_dirty_client(const struct client *c)
{
	if (c)
		chara_tiling_dirty(c->scr, c->ws);
}

void
chara_tiling_dirty_all(void)
{
	struct screen *s;

	wl_list_for_each(s, &wm.screens, link)
		for (uint8_t ws = 1; ws <= CHARA_WORKSPACES; ++ws)
			chara_tiling_dirty(s, ws);
}

void
chara_tiling_flush(void)
{
	drain(NULL);
}

bool
chara_tiling_init(struct wl_event_loop *loop)
{
	event_loop = loop;
	return true;
}

void
chara_tiling_finish(void)
{
	event_loop = NULL;
}

/*
 * Whether focusing a window has to raise it as well.
 *
 * On a monocle workspace, yes, and for floating windows as much as tiled ones.
 * Monocle stacks every window on the same rectangle, so one that is focused
 * but still behind another is a window that has not appeared -- and a floating
 * window that did not raise with the focus would be buried by the next tiled
 * one that did.
 *
 * The other layouts put their windows side by side, where raising would
 * disturb the stacking order and show nothing that was not already visible.
 */
bool
chara_tiling_focus_raises(const struct client *c)
{
	if (!c || c->minimized || !c->scr)
		return false;
	if (c->ws < 1 || c->ws > CHARA_WORKSPACES || c->ws != c->scr->ws)
		return false;
	return c->scr->tiles[c->ws].layout == TILE_MONOCLE;
}

bool
chara_tiling_ignore_enter(void)
{
	int32_t x, y;

	if (!settling || config.tiling.focus_follows_relayout)
		return false;
	if (!swc_cursor_position(&x, &y))
		return false;
	if (x != settled_x || y != settled_y) {
		settling = false;   /* the pointer really has moved since */
		return false;
	}
	return true;
}

/* -------------------------------------------------------- membership */

static void
remember_floating(struct client *c)
{
	struct swc_rectangle g;

	/* Only a window that is standing on its own has a place worth keeping:
	 * a tiled one is in a cell, and a fullscreen or maximized one is
	 * wearing the whole monitor. */
	if (c->tiled || c->fullscreen || c->maximized)
		return;
	if (swc_window_get_geometry(c->win, &g) && g.width && g.height)
		c->floating = g;
	else
		c->floating = (struct swc_rectangle){ c->x, c->y, c->width, c->height };
}

static void
restore_floating(struct client *c)
{
	struct swc_rectangle g = c->floating;

	c->tile_mode_set = false;
	c->tile_edges = 0;
	if (!g.width || !g.height) {
		g.x = c->x;
		g.y = c->y;
		g.width = c->width ? c->width : 640;
		g.height = c->height ? c->height : 480;
	}
	/* Recorded either way, because a window that is fullscreen now will be
	 * put back by whoever ends that, and this is where it should land. */
	c->x = g.x;
	c->y = g.y;
	c->width = g.width;
	c->height = g.height;
	/* Saying "stacked" to a fullscreen window would take it out of
	 * fullscreen; that mode owns it until something ends it. */
	if (c->fullscreen || c->maximized)
		return;
	swc_window_set_stacked(c->win);
	swc_window_set_geometry(c->win, &g);
}

void
chara_tiling_admit(struct client *c)
{
	if (!c || c->tiled)
		return;
	remember_floating(c);
	c->tiled = true;
	c->tile_mode_set = false;
	c->tile_edges = 0;
	if (!(c->tile_main > 0))
		c->tile_main = 1.0;
	if (!(c->tile_cross > 0))
		c->tile_cross = 1.0;
	place_in_order(c);
	chara_tiling_dirty_client(c);
}

void
chara_tiling_forget(struct client *c)
{
	struct screen *s;
	uint8_t ws;

	if (!c || !c->tiled)
		return;
	s = c->scr;
	ws = c->ws;
	c->tiled = false;
	c->tile_mode_set = false;
	c->tile_edges = 0;
	if (s) {
		renumber(s, ws);
		chara_tiling_dirty(s, ws);
	}
}

bool
chara_tiling_set(struct client *c, bool tiled)
{
	if (!c || c->tiled == tiled)
		return false;
	if (tiled) {
		/* Maximized is a mode of its own; a window cannot be in two. */
		chara_set_maximized(c, false);
		chara_tiling_admit(c);
	} else {
		chara_tiling_forget(c);
		restore_floating(c);
		swc_window_raise(c->win);
	}
	return true;
}

/*
 * Take a window out of the tiling and leave it exactly where it is.
 *
 * This is what a drag wants. swc starts an interactive move from the geometry
 * the window has at that instant, and a window that has just been told to go
 * back to a size it had some time ago has not got there yet -- the drag would
 * begin from the wrong offset and the window would jump. Making the remembered
 * floating geometry the cell it is sitting in means there is nothing to go
 * back to, and it peels off the grid where the pointer left it.
 */
bool
chara_tiling_release_in_place(struct client *c)
{
	struct swc_rectangle g;

	if (!c || !c->tiled)
		return false;
	/* Unless it is wearing the whole monitor, in which case where it is is
	 * not a place worth going back to. */
	if (!c->fullscreen && !c->maximized) {
		if (swc_window_get_geometry(c->win, &g) && g.width && g.height)
			c->floating = g;
		else
			c->floating = (struct swc_rectangle){ c->x, c->y,
			                                      c->width, c->height };
	}
	return chara_tiling_set(c, false);
}

void
chara_tiling_restore_mode(struct client *c)
{
	if (!c || !c->tiled)
		return;
	/* Something else held the window -- a fullscreen, a maximize -- and has
	 * given it back, so the tiled mode and the tiled edges have to be said
	 * again before the geometry will mean anything. */
	c->tile_mode_set = false;
	c->tile_edges = 0;
	chara_tiling_dirty_client(c);
}

void
chara_tiling_reseat(struct client *c, struct screen *from, uint8_t from_ws)
{
	if (!c || !c->tiled)
		return;
	if (from == c->scr && from_ws == c->ws) {
		chara_tiling_dirty_client(c);
		return;
	}
	/* `from` is null when the monitor it was on has gone; there is nothing
	 * left to renumber there, but it still needs a place here. */
	if (from) {
		renumber(from, from_ws);
		chara_tiling_dirty(from, from_ws);
	}
	place_in_order(c);
	chara_tiling_dirty_client(c);
}

/* --------------------------------------------------------- monitors */

/* The monitor lying that way, if there is one. Monitors are rectangles and so
 * are cells, so the neighbour rule that works for one works for the other. */
static struct screen *
screen_toward(struct screen *from, enum tile_dir dir)
{
	struct tile_item boxes[MAX_SCREENS];
	struct screen *list[MAX_SCREENS], *s;
	unsigned n = 0;
	int self = -1, other;

	wl_list_for_each(s, &wm.screens, link) {
		if (n == MAX_SCREENS)
			break;
		tile_item_init(&boxes[n]);
		boxes[n].rect = (struct swc_rectangle){ s->x, s->y, s->width, s->height };
		list[n] = s;
		if (s == from)
			self = (int)n;
		++n;
	}
	if (self < 0)
		return NULL;
	other = tile_neighbour(boxes, n, (unsigned)self, dir);
	return other < 0 ? NULL : list[other];
}

static bool
move_to_screen(struct client *c, struct screen *to)
{
	struct screen *from = c->scr;
	uint8_t from_ws = c->ws;

	if (!to || to == from)
		return false;
	chara_forget_focus(c, to);
	c->scr = to;
	if (c->ws != to->ws) {
		c->ws = to->ws;
		swc_window_set_workspace(c->win, c->ws);
		chara_sync_windows();
	}
	chara_tiling_reseat(c, from, from_ws);
	if (wm.cur == c)
		to->focus = c;
	return true;
}

/* -------------------------------------------------------- navigation */

/*
 * Everything a monitor is showing, with the rectangle it actually occupies.
 *
 * Not the tiling's plan: "the window to the left" is a question about what is
 * on the screen, and a floating window is on the screen. Directional focus
 * therefore works in a floating session too, and reads the same in both.
 */
static unsigned
gather_showing(const struct screen *s, struct client **out,
               struct tile_item *items, unsigned max)
{
	struct client *c;
	unsigned n = 0;

	if (!s)
		return 0;
	wl_list_for_each(c, &wm.clients, link) {
		struct swc_rectangle g;

		if (c->scr != s || c->ws != s->ws || c->minimized || !c->visible)
			continue;
		if (n == max)
			break;
		if (!swc_window_get_geometry(c->win, &g) || !g.width || !g.height)
			g = (struct swc_rectangle){ c->x, c->y, c->width, c->height };
		tile_item_init(&items[n]);
		items[n].rect = g;
		out[n] = c;
		++n;
	}
	return n;
}

/* The window a monitor should hand the focus to when it is stepped onto. */
static struct client *
first_showing(struct screen *s)
{
	struct client *clients[TILE_MAX_WINDOWS];
	struct tile_item items[TILE_MAX_WINDOWS];
	unsigned n = gather_showing(s, clients, items, TILE_MAX_WINDOWS);

	if (!n)
		return NULL;
	if (s->focus && s->focus->scr == s && s->focus->ws == s->ws &&
	    !s->focus->minimized)
		return s->focus;
	return clients[0];
}

/*
 * Stepping through a monocle workspace.
 *
 * Nothing is to the left of anything there -- every window is on the same
 * rectangle -- so a direction has to mean something else, and going back and
 * forth through the order is what it means. Both ends wrap, so the same key
 * keeps going round rather than stopping at a window that looks no different
 * from the others.
 */
static bool
monocle_step(struct screen *s, enum tile_dir dir)
{
	struct plan p;
	unsigned next;
	int self;

	if (!s || s->ws < 1 || s->ws > CHARA_WORKSPACES)
		return false;
	if (s->tiles[s->ws].layout != TILE_MONOCLE)
		return false;
	if (!build_plan(s, s->ws, &p) || p.n < 2)
		return false;
	self = wm.cur ? index_of(&p, wm.cur) : -1;
	if (self < 0)
		next = 0;
	else if (dir == TILE_RIGHT || dir == TILE_DOWN)
		next = ((unsigned)self + 1) % p.n;
	else
		next = ((unsigned)self + p.n - 1) % p.n;
	chara_focus(p.clients[next]);
	return true;
}

bool
chara_focus_dir(enum tile_dir dir)
{
	struct client *clients[TILE_MAX_WINDOWS];
	struct tile_item items[TILE_MAX_WINDOWS];
	struct screen *s = chara_active_screen(), *to;
	unsigned n;
	int self, other;

	if (!s)
		return false;
	n = gather_showing(s, clients, items, TILE_MAX_WINDOWS);
	self = -1;
	for (unsigned i = 0; i < n && self < 0; ++i)
		if (clients[i] == wm.cur)
			self = (int)i;
	if (self < 0) {
		/* Nothing here has the focus: take it rather than doing nothing. */
		if (n) {
			chara_focus(clients[0]);
			return true;
		}
	} else {
		other = tile_neighbour(items, n, (unsigned)self, dir);
		if (other >= 0) {
			chara_focus(clients[other]);
			return true;
		}
	}
	/* Nothing beside it, which in monocle is always true: step through the
	 * order there instead of walking off the monitor. */
	if (monocle_step(s, dir))
		return true;
	/* Off the edge of this monitor, so carry on to the next one. */
	to = screen_toward(s, dir);
	if (!to || to == s)
		return false;
	{
		struct client *arrive = first_showing(to);

		if (!arrive)
			return false;
		wm.scr = to;
		chara_focus(arrive);
		return true;
	}
}

bool
chara_tiling_move_dir(struct client *c, enum tile_dir dir)
{
	struct plan p;
	int self, other;

	if (!c || !c->tiled || !c->scr || !build_plan(c->scr, c->ws, &p))
		return false;
	self = index_of(&p, c);
	if (self < 0)
		return false;
	other = tile_neighbour(p.items, p.n, (unsigned)self, dir);
	if (other < 0)
		return move_to_screen(c, screen_toward(c->scr, dir));
	swap_places(c, p.clients[other]);
	chara_tiling_dirty(c->scr, c->ws);
	return true;
}

bool
chara_tiling_swap(struct client *a, struct client *b)
{
	if (!a || !b || a == b || !a->tiled || !b->tiled)
		return false;
	if (a->scr != b->scr || a->ws != b->ws)
		return false;
	swap_places(a, b);
	chara_tiling_dirty(a->scr, a->ws);
	return true;
}

/*
 * Send a window to the front of the order, which in the master layout is the
 * master slot. A window already at the front trades with the one behind it, so
 * the same key brings a window out and puts it back.
 */
bool
chara_tiling_promote(struct client *c)
{
	struct plan p;
	int self;

	if (!c || !c->tiled || !c->scr || !build_plan(c->scr, c->ws, &p) || p.n < 2)
		return false;
	self = index_of(&p, c);
	if (self < 0)
		return false;
	swap_places(c, p.clients[self == 0 ? 1 : 0]);
	chara_tiling_dirty(c->scr, c->ws);
	return true;
}

/*
 * The tiled window whose cell holds a point, and where that cell is.
 *
 * The cell rather than the window: a drag wants to show where the window would
 * land, frame and all, not where its content would start.
 */
struct client *
chara_tiling_at(int32_t x, int32_t y, struct swc_rectangle *cell)
{
	struct screen *s = chara_screen_at(x, y);
	struct plan p;
	int at;

	if (!s || !build_plan(s, s->ws, &p))
		return NULL;
	at = tile_at(p.items, p.n, x, y);
	if (at < 0)
		return NULL;
	if (cell)
		*cell = p.items[at].rect;
	return p.clients[at];
}

bool
chara_tiling_drop_at(struct client *c, int32_t x, int32_t y)
{
	struct screen *s = chara_screen_at(x, y);
	struct client *over;
	bool moved = false;

	if (!c || !c->tiled || !s)
		return false;
	if (s != c->scr) {
		/* Dropped on another monitor: it belongs there now, and to the
		 * workspace that monitor is showing. */
		if (!move_to_screen(c, s))
			return false;
		moved = true;
	}
	over = chara_tiling_at(x, y, NULL);
	if (!over || over == c)
		return moved;   /* the monitor move on its own still counts */
	swap_places(c, over);
	chara_tiling_dirty(s, s->ws);
	return true;
}

/* ------------------------------------------------------------ resize */

bool
chara_tiling_resize_dir(struct client *c, enum tile_dir dir, int32_t pixels)
{
	struct plan p;
	int self;

	if (!c || !c->tiled || !c->scr || !pixels)
		return false;
	if (!build_plan(c->scr, c->ws, &p))
		return false;
	self = index_of(&p, c);
	if (self < 0)
		return false;
	if (!tile_resize(&c->scr->tiles[c->ws], p.items, p.n, (unsigned)self, dir,
	                 pixels, p.area, p.inner, p.outer))
		return false;
	write_back(&p);
	chara_tiling_dirty(c->scr, c->ws);
	return true;
}

/* ------------------------------------------------------------ layout */

struct tile_ws *
chara_tiling_ws(struct screen *s, uint8_t ws)
{
	if (!s || ws < 1 || ws > CHARA_WORKSPACES)
		return NULL;
	return &s->tiles[ws];
}

unsigned
chara_tiling_count(struct screen *s, uint8_t ws)
{
	struct client *members[TILE_MAX_WINDOWS];

	return collect(s, ws, false, members, TILE_MAX_WINDOWS);
}

void
chara_tiling_set_layout(struct screen *s, uint8_t ws, enum tile_layout layout)
{
	struct tile_ws *t = chara_tiling_ws(s, ws);

	if (!t || layout >= TILE_LAYOUT_LAST || t->layout == layout)
		return;
	t->layout = layout;
	chara_tiling_dirty(s, ws);
}

void
chara_tiling_cycle_layout(struct screen *s, uint8_t ws, int direction)
{
	struct tile_ws *t = chara_tiling_ws(s, ws);
	int next;

	if (!t)
		return;
	next = (int)t->layout + (direction < 0 ? -1 : 1);
	if (next < 0)
		next = TILE_LAYOUT_LAST - 1;
	if (next >= (int)TILE_LAYOUT_LAST)
		next = 0;
	chara_tiling_set_layout(s, ws, (enum tile_layout)next);
}

bool
chara_tiling_master_count(struct screen *s, uint8_t ws, int32_t delta)
{
	struct tile_ws *t = chara_tiling_ws(s, ws);
	int32_t want;

	if (!t)
		return false;
	want = (int32_t)t->master_count + delta;
	if (want < 1)
		want = 1;
	if (want > TILE_MAX_WINDOWS)
		want = TILE_MAX_WINDOWS;
	if ((unsigned)want == t->master_count)
		return false;
	t->master_count = (unsigned)want;
	chara_tiling_dirty(s, ws);
	return true;
}

bool
chara_tiling_master_ratio(struct screen *s, uint8_t ws, int32_t percent)
{
	struct tile_ws *t = chara_tiling_ws(s, ws);
	double want;

	if (!t)
		return false;
	want = t->master_ratio + (double)percent / 100.0;
	if (want < 0.05)
		want = 0.05;
	if (want > 0.95)
		want = 0.95;
	if (fabs(want - t->master_ratio) < 1e-9)
		return false;
	t->master_ratio = want;
	chara_tiling_dirty(s, ws);
	return true;
}

void
chara_tiling_equalize(struct screen *s, uint8_t ws)
{
	struct client *members[TILE_MAX_WINDOWS];
	struct tile_ws *t = chara_tiling_ws(s, ws);
	unsigned n = collect(s, ws, false, members, TILE_MAX_WINDOWS);

	if (!t)
		return;
	/* Every member, not only the ones on the grid: one that is minimized
	 * should come back the same size as everything else, not the size it
	 * was stretched to before it went. */
	for (unsigned i = 0; i < n; ++i) {
		members[i]->tile_main = 1.0;
		members[i]->tile_cross = 1.0;
	}
	t->master_ratio = 0.5;
	chara_tiling_dirty(s, ws);
}

void
chara_tiling_ws_reset(struct screen *s)
{
	if (!s)
		return;
	for (uint8_t ws = 0; ws <= CHARA_WORKSPACES; ++ws) {
		tile_ws_init(&s->tiles[ws]);
		s->tiles[ws].layout = config.tiling.layout;
		s->tiles[ws].master_side = config.tiling.master_side;
		s->tiles[ws].master_ratio = config.tiling.master_ratio;
		s->tiles[ws].master_count = config.tiling.master_count;
		s->tile_dirty[ws] = false;
	}
}

/*
 * A monitor that has just appeared.
 *
 * Whether a workspace tiles is the session's, which is why the reset above
 * leaves it alone: a reload is "apply what I have written" for the layouts,
 * not an order to float everything the user has spent the session tiling.
 * A monitor with no session behind it takes the configured default instead.
 */
void
chara_tiling_ws_init(struct screen *s)
{
	if (!s)
		return;
	chara_tiling_ws_reset(s);
	for (uint8_t ws = 0; ws <= CHARA_WORKSPACES; ++ws)
		s->tile_on[ws] = config.tiling.enabled;
}

bool
chara_tiling_ws_enabled(const struct screen *s, uint8_t ws)
{
	if (!s || ws < 1 || ws > CHARA_WORKSPACES)
		return false;
	return s->tile_on[ws];
}

/*
 * Turn one workspace's tiling on or off.
 *
 * The flag decides what happens to windows opening there later; the windows
 * already on it are taken in or out to match, because a workspace that says it
 * tiles and shows a screen of floating windows is not telling the truth. A
 * window a rule has spoken for keeps what the rule gave it -- a launcher is
 * meant to float wherever it is opened.
 */
void
chara_tiling_ws_enable(struct screen *s, uint8_t ws, bool on)
{
	struct client *c;

	if (!s || ws < 1 || ws > CHARA_WORKSPACES)
		return;
	s->tile_on[ws] = on;
	wl_list_for_each(c, &wm.clients, link) {
		if (c->scr != s || c->ws != ws || c->tile_ruled)
			continue;
		chara_tiling_set(c, on);
	}
	chara_tiling_dirty(s, ws);
}
