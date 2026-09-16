#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input-event-codes.h>

#include "config.h"

static const struct swc_window_handler win_handler;

/* ------------------------------------------------------------------ ids */

void
chara_client_label(const struct client *c, char *out, size_t size)
{
	if (!c) {
		snprintf(out, size, "none");
	} else if (!c->name[0]) {
		snprintf(out, size, "#%u", c->id);
	} else if (c->ordinal > 1) {
		snprintf(out, size, "%s:%u", c->name, c->ordinal);
	} else {
		snprintf(out, size, "%s", c->name);
	}
}

/* Lowest ordinal not currently taken by another window of the same name. */
static unsigned
next_ordinal(const char *name, const struct client *self)
{
	for (unsigned n = 1; n < 1024; ++n) {
		struct client *c;
		bool taken = false;
		wl_list_for_each(c, &wm.clients, link)
			if (c != self && c->ordinal == n && !strcmp(c->name, name))
				taken = true;
		if (!taken)
			return n;
	}
	return 0;
}

struct client *
chara_lookup(const char *selector)
{
	struct client *c;

	if (!selector || !*selector || !strcmp(selector, "focused"))
		return wm.cur;

	if (*selector == '#') {
		char *end;
		unsigned long id = strtoul(selector + 1, &end, 10);
		if (*end)
			return NULL;
		wl_list_for_each(c, &wm.clients, link)
			if (c->id == id)
				return c;
		return NULL;
	}

	char name[CHARA_NAME_MAX];
	unsigned ordinal = 1;
	const char *colon = strchr(selector, ':');
	size_t length = colon ? (size_t)(colon - selector) : strlen(selector);
	if (length == 0 || length >= sizeof(name))
		return NULL;
	memcpy(name, selector, length);
	name[length] = '\0';
	if (colon) {
		char *end;
		unsigned long n = strtoul(colon + 1, &end, 10);
		if (*end || !n)
			return NULL;
		ordinal = (unsigned)n;
	}
	wl_list_for_each(c, &wm.clients, link)
		if (c->ordinal == ordinal && !strcmp(c->name, name))
			return c;
	return NULL;
}

/* -------------------------------------------------------------- screens */

struct screen *
chara_screen_at(int32_t x, int32_t y)
{
	struct screen *s;

	wl_list_for_each(s, &wm.screens, link) {
		if (x >= s->x && y >= s->y && x < s->x + (int32_t)s->width &&
		    y < s->y + (int32_t)s->height)
			return s;
	}
	return NULL;
}

struct screen *
chara_active_screen(void)
{
	int32_t x, y;

	if (swc_cursor_position(&x, &y)) {
		struct screen *s = chara_screen_at(x / 256, y / 256);
		if (s)
			return s;
	}
	return wm.scr;
}

uint8_t
chara_active_ws(void)
{
	struct screen *s = chara_active_screen();
	return s ? s->ws : 1;
}

struct screen *
chara_window_screen(const struct client *c)
{
	struct swc_rectangle g;
	if (!c || !c->win || !swc_window_get_geometry(c->win, &g))
		return c ? c->scr : NULL;

	struct screen *s = chara_screen_at(g.x + (int32_t)g.width / 2,
	                                   g.y + (int32_t)g.height / 2);
	return s ? s : c->scr;
}

/* ---------------------------------------------------------------- focus */

void
chara_focus(struct client *c)
{
	struct client *previous = wm.cur;

	if (c && c->scr)
		c->scr->focus = c;
	/* Applying a decoration resets the compositor's titlebar hover state, so
	 * redrawing a window that is already focused would clear the highlight
	 * and re-render the bar on every pointer motion across it. */
	if (previous != c) {
		wm.cur = c;
		if (previous)
			chara_decorate(previous, false);
		if (c)
			chara_decorate(c, true);
	}
	swc_window_focus(c ? c->win : NULL);
}

static bool
on_workspace(const struct client *c, const struct screen *s)
{
	return c && !c->minimized && c->scr == s && c->ws == s->ws;
}

static struct client *
first_on(struct screen *s)
{
	struct client *c;

	if (s->focus && on_workspace(s->focus, s))
		return s->focus;
	wl_list_for_each(c, &wm.clients, link)
		if (on_workspace(c, s))
			return c;
	return NULL;
}

void
chara_sync_windows(void)
{
	struct client *c;

	wl_list_for_each(c, &wm.clients, link) {
		bool visible = c->scr && !c->minimized && c->ws == c->scr->ws;
		if (visible == c->visible)
			continue;
		c->visible = visible;
		if (visible)
			swc_window_show(c->win);
		else
			swc_window_hide(c->win);
	}
}

void
chara_focus_step(int direction)
{
	struct screen *s = chara_active_screen();
	struct client *c, *first = NULL, *last = NULL, *previous = NULL, *next = NULL;
	bool seen = false;

	if (!s)
		return;
	wl_list_for_each(c, &wm.clients, link) {
		if (!on_workspace(c, s))
			continue;
		if (!first)
			first = c;
		last = c;
		if (seen && !next)
			next = c;
		if (c == wm.cur)
			seen = true;
		else if (!seen)
			previous = c;
	}
	if (!first)
		return;
	if (direction > 0)
		chara_focus(next ? next : first);
	else
		chara_focus(previous ? previous : last);
}

/* ----------------------------------------------------------- workspaces */

void
chara_ws_go_to(struct screen *s, uint8_t ws)
{
	if (!s || ws < 1 || ws > CHARA_WORKSPACES || ws == s->ws)
		return;

	s->ws = ws;
	/* Tell desktop shells which workspace this monitor is showing. */
	if (s->scr)
		swc_workspace_set_active(s->scr, ws);
	chara_sync_windows();
	chara_layout_apply(s, ws);
	chara_focus(first_on(s));
}

void
chara_ws_move_to(uint8_t ws, struct client *c)
{
	uint8_t from;

	if (!c || ws < 1 || ws > CHARA_WORKSPACES || c->ws == ws)
		return;

	from = c->ws;
	c->ws = ws;
	swc_window_set_workspace(c->win, ws);
	chara_sync_windows();
	if (c->scr) {
		chara_layout_apply(c->scr, from);
		chara_layout_apply(c->scr, ws);
		if (wm.cur == c)
			chara_focus(first_on(c->scr));
	}
}

/* ---------------------------------------------------------- window state */

void
chara_update_mode_geometry(struct client *c)
{
	struct screen *s = c->scr;

	if (!s || !s->scr)
		return;
	if (c->fullscreen) {
		swc_window_set_fullscreen(c->win, s->scr);
	} else if (c->maximized) {
		/* Decorations are drawn outside the content, so filling the usable
		 * area exactly would push them off the screen. Leave room for the
		 * ones the configuration asks to keep visible. */
		struct swc_rectangle g = s->scr->usable_geometry;
		int32_t side = config.values.maximize_borders ? chara_border_width() : 0;
		int32_t top = side + (config.values.maximize_titlebar
		    ? chara_titlebar_height(c) : 0);

		g.x += side;
		g.y += top;
		g.width = g.width > (uint32_t)(2 * side) ? g.width - 2 * side : 1;
		g.height = g.height > (uint32_t)(top + side) ? g.height - top - side : 1;
		swc_window_set_geometry(c->win, &g);
	} else if (c->tiled) {
		chara_layout_apply(s, c->ws);
	}
}

/* Put a window back into whichever mode it was in before fullscreen. */
static void
restore_mode(struct client *c)
{
	if (c->maximized) {
		if (!c->tiled)
			swc_window_set_tiled(c->win);
		chara_update_mode_geometry(c);
	} else if (c->tiled) {
		swc_window_set_tiled(c->win);
		chara_layout_apply(c->scr, c->ws);
	} else {
		swc_window_set_stacked(c->win);
		struct swc_rectangle g = { c->x, c->y, c->width, c->height };
		swc_window_set_geometry(c->win, &g);
	}
}

bool
chara_set_fullscreen(struct client *c, bool fullscreen, struct swc_screen *on)
{
	if (!c || c->fullscreen == fullscreen)
		return false;

	c->fullscreen = fullscreen;
	if (fullscreen) {
		struct screen *s = on ? chara_screen_of(on) : c->scr;
		if (s)
			c->scr = s;
		swc_window_set_fullscreen(c->win, c->scr ? c->scr->scr : NULL);
	} else {
		/* A client leaving fullscreen goes back to maximized or tiled if
		 * that is where it came from, not to a bare floating window. */
		restore_mode(c);
	}
	if (c->scr && !c->maximized)
		chara_layout_apply(c->scr, c->ws);
	chara_decorate(c, wm.cur == c);
	return true;
}

bool
chara_set_maximized(struct client *c, bool maximized)
{
	if (!c || c->maximized == maximized || c->fullscreen)
		return false;

	c->maximized = maximized;
	restore_mode(c);
	chara_decorate(c, wm.cur == c);
	return true;
}

void
chara_minimize(struct client *c)
{
	if (!c || c->minimized)
		return;

	c->minimized = ++wm.minimize_order;
	swc_window_set_minimized(c->win, true);
	chara_sync_windows();
	if (c->scr)
		chara_layout_apply(c->scr, c->ws);
	if (wm.cur == c) {
		if (c->scr && c->scr->focus == c)
			c->scr->focus = NULL;
		chara_focus(c->scr ? first_on(c->scr) : NULL);
	}
}

void
chara_restore(struct client *c)
{
	if (!c) {
		/* No window given: bring back the most recently minimized one. */
		struct client *candidate = NULL, *other;
		wl_list_for_each(other, &wm.clients, link)
			if (other->minimized &&
			    (!candidate || other->minimized > candidate->minimized))
				candidate = other;
		c = candidate;
	}
	if (!c || !c->minimized)
		return;

	c->minimized = 0;
	swc_window_set_minimized(c->win, false);
	if (c->scr)
		c->ws = c->scr->ws;
	swc_window_set_workspace(c->win, c->ws);
	chara_sync_windows();
	if (c->scr)
		chara_layout_apply(c->scr, c->ws);
	chara_focus(c);
}

/* ---------------------------------------------------------------- rules */

static void
apply_rule(struct client *c)
{
	struct rule *r, *match = NULL;
	const char *app_id = c->win->app_id;

	if (!app_id)
		return;
	wl_list_for_each(r, &config.rules, link)
		if (!strcmp(r->app_id, app_id))
			match = r;
	if (!match)
		return;

	if (match->name[0] && strcmp(c->name, match->name)) {
		snprintf(c->name, sizeof(c->name), "%s", match->name);
		c->ordinal = next_ordinal(c->name, c);
	}
	c->movable = match->movable;
	c->resizable = match->resizable;
	if (match->has_titlebar)
		c->titlebar = match->titlebar;
	if (match->has_floating && match->floating)
		chara_layout_set_floating(c, true);

	if (c->tiled || c->fullscreen || c->maximized)
		return;

	struct swc_rectangle g = { c->x, c->y, c->width, c->height };
	if (match->width) {
		g.width = match->width;
		g.height = match->height;
	}
	if (match->has_pos) {
		g.x = match->x;
		g.y = match->y;
	} else if (match->center && c->scr && c->scr->scr) {
		struct swc_rectangle area = c->scr->scr->usable_geometry;
		g.x = area.x + ((int32_t)area.width - (int32_t)g.width) / 2;
		g.y = area.y + ((int32_t)area.height - (int32_t)g.height) / 2;
	}
	swc_window_set_geometry(c->win, &g);
	c->x = g.x;
	c->y = g.y;
	c->width = g.width;
	c->height = g.height;
}

/* --------------------------------------------------------- swc callbacks */

static void
on_title(void *data)
{
	struct client *c = data;
	chara_decorate(c, wm.cur == c);
}

static void
on_app_id(void *data)
{
	struct client *c = data;
	apply_rule(c);
	chara_decorate(c, wm.cur == c);
}

static void
on_entered(void *data)
{
	struct client *c = data;

	if (wm.grab.active || !c->visible)
		return;
	if (c->scr)
		wm.scr = c->scr;
	chara_focus(c);
}

static void
on_destroy(void *data)
{
	struct client *c = data;
	struct screen *s = c->scr;

	if (wm.grab.client == c) {
		wm.grab.active = false;
		wm.grab.client = NULL;
	}
	if (s && s->focus == c)
		s->focus = NULL;
	if (wm.cur == c)
		wm.cur = NULL;

	uint8_t ws = c->ws;
	if (c->tiled)
		wl_list_remove(&c->tile_link);
	wl_list_remove(&c->link);
	free(c);

	if (s) {
		chara_layout_apply(s, ws);
		chara_focus(first_on(s));
	}
}

static void
on_titlebar_action(void *data, enum swc_titlebar_action action)
{
	struct client *c = data;

	switch (action) {
	case SWC_TITLEBAR_FOCUS:
		chara_focus(c);
		break;
	case SWC_TITLEBAR_MINIMIZE:
		chara_minimize(c);
		break;
	case SWC_TITLEBAR_FULLSCREEN:
		if (config.decoration->fullscreen_action == TITLEBAR_MAXIMIZE)
			chara_set_maximized(c, !c->maximized);
		else
			chara_set_fullscreen(c, !c->fullscreen, NULL);
		break;
	case SWC_TITLEBAR_CLOSE:
		swc_window_close(c->win);
		break;
	}
}

static void
on_request_activate(void *data)
{
	struct client *c = data;

	if (c->minimized)
		chara_restore(c);
	else if (c->scr && c->ws != c->scr->ws)
		chara_ws_go_to(c->scr, c->ws);
	chara_focus(c);
}

static void
on_request_minimized(void *data, bool minimized)
{
	if (minimized)
		chara_minimize(data);
	else
		chara_restore(data);
}

static void
on_request_maximized(void *data, bool maximized)
{
	chara_set_maximized(data, maximized);
}

static void
on_request_fullscreen(void *data, bool fullscreen, struct swc_screen *screen)
{
	chara_set_fullscreen(data, fullscreen, screen);
}

/* A tiled window asking to be dragged becomes floating first. */
static void
on_request_move(void *data)
{
	struct client *c = data;
	if (c->movable)
		chara_layout_set_floating(c, true);
}

static void
on_request_resize(void *data)
{
	struct client *c = data;
	if (c->resizable)
		chara_layout_set_floating(c, true);
}

static const struct swc_window_handler win_handler = {
	.destroy = on_destroy,
	.title_changed = on_title,
	.app_id_changed = on_app_id,
	.entered = on_entered,
	.move = on_request_move,
	.resize = on_request_resize,
	.titlebar_action = on_titlebar_action,
	.request_activate = on_request_activate,
	.request_minimized = on_request_minimized,
	.request_maximized = on_request_maximized,
	.request_fullscreen = on_request_fullscreen,
};

void
chara_new_window(struct swc_window *win)
{
	struct client *c = calloc(1, sizeof(*c));
	struct screen *s = chara_active_screen();

	if (!c)
		_err(1, "couldn't allocate a window");

	win->motion_throttle_ms = 1000 / 85;
	win->min_width = 1;
	win->min_height = 1;

	c->win = win;
	c->scr = s;
	c->ws = s ? s->ws : 1;
	c->id = ++wm.last_id;
	c->visible = true;
	c->movable = true;
	c->resizable = true;
	c->titlebar = true;
	c->width = 640;
	c->height = 480;

	wl_list_insert(wm.clients.prev, &c->link);
	swc_window_set_handler(win, &win_handler, c);
	swc_window_set_workspace(win, c->ws);

	if (!chara_layout_admit(c)) {
		/* Floating windows open in the middle of the active monitor. */
		swc_window_set_stacked(c->win);
		if (s && s->scr) {
			struct swc_rectangle area = s->scr->usable_geometry;
			c->x = area.x + ((int32_t)area.width - (int32_t)c->width) / 2;
			c->y = area.y + ((int32_t)area.height - (int32_t)c->height) / 2;
		}
		struct swc_rectangle g = { c->x, c->y, c->width, c->height };
		swc_window_set_geometry(win, &g);
	}

	apply_rule(c);
	if (s)
		chara_layout_apply(s, c->ws);
	swc_window_show(win);
	chara_focus(c);
}

/* ----------------------------------------------------------- mod + drag */

static void
move_handler(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	(void)data, (void)time, (void)value;
	struct client *c = wm.cur;

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (wm.grab.active && !wm.grab.resize && wm.grab.client) {
			swc_window_end_move(wm.grab.client->win);
			struct swc_rectangle g;
			if (swc_window_get_geometry(wm.grab.client->win, &g)) {
				wm.grab.client->x = g.x;
				wm.grab.client->y = g.y;
			}
			wm.grab = (struct grab){0};
		}
		return;
	}
	if (!c || !c->movable || c->fullscreen)
		return;
	chara_layout_set_floating(c, true);
	wm.grab = (struct grab){ .active = true, .resize = false, .client = c };
	swc_window_begin_move(c->win);
}

static void
resize_handler(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	(void)data, (void)time, (void)value;
	struct client *c = wm.cur;

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (wm.grab.active && wm.grab.resize && wm.grab.client) {
			swc_window_end_resize(wm.grab.client->win);
			struct swc_rectangle g;
			if (swc_window_get_geometry(wm.grab.client->win, &g)) {
				wm.grab.client->width = g.width;
				wm.grab.client->height = g.height;
			}
			wm.grab = (struct grab){0};
		}
		return;
	}
	if (!c || !c->resizable || c->fullscreen)
		return;
	chara_layout_set_floating(c, true);
	chara_set_maximized(c, false);
	wm.grab = (struct grab){ .active = true, .resize = true, .client = c };
	swc_window_begin_resize(c->win, SWC_WINDOW_EDGE_AUTO);
}

void
chara_bind_mouse(uint32_t mod)
{
	swc_add_binding(SWC_BINDING_BUTTON, mod, BTN_LEFT, move_handler, NULL);
	swc_add_binding(SWC_BINDING_BUTTON, mod, BTN_RIGHT, resize_handler, NULL);
}

/* --------------------------------------------------------------- actions */

void
chara_action_run(const struct action *a)
{
	char idbuf[CHARA_NAME_MAX + 16], *argv[6];
	char numbers[4][16];
	int argc = 0;

	const struct command *cmd = &commands[a->command];
	if (cmd->selects) {
		if (a->selector) {
			argv[argc++] = a->selector;
		} else {
			chara_client_label(wm.cur, idbuf, sizeof(idbuf));
			argv[argc++] = wm.cur ? idbuf : (char *)"focused";
		}
	}
	if (a->text) {
		argv[argc++] = a->text;
	} else {
		for (unsigned i = 0; i < a->argc && i < 4; ++i) {
			snprintf(numbers[i], sizeof(numbers[i]), "%d", a->args[i]);
			argv[argc++] = numbers[i];
		}
	}
	argv[argc] = NULL;
	status result = chara_ipc_dispatch(cmd, argc, argv);
	if (!result.ok)
		_wrn("%s: %s", cmd->name, result.msg);
}
