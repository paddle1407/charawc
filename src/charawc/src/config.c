#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "config.h"

#if LUA_VERSION_NUM < 502
#error charawc requires Lua 5.2 or newer
#endif

/* Parsing runs as a protected Lua call. Attach allocations to their owner
 * before any later Lua call can throw, so failure cleans up like success. */

static void
table(lua_State *L, int index, const char *path)
{
	if (!lua_istable(L, index))
		luaL_error(L, "%s: expected a table", path);
	if (lua_getmetatable(L, index))
		luaL_error(L, "%s: configuration tables must not have metatables", path);
}

static const char *
string(lua_State *L, int index, const char *path, bool empty)
{
	size_t length;
	if (lua_type(L, index) != LUA_TSTRING)
		luaL_error(L, "%s: expected a string", path);
	const char *s = lua_tolstring(L, index, &length);
	if ((!empty && !length) || length > 65536 || memchr(s, '\0', length))
		luaL_error(L, "%s: invalid string (empty, too long or contains NUL)", path);
	return s;
}

static void
fields(lua_State *L, int index, const char *path, const char *const *names)
{
	index = lua_absindex(L, index);
	table(L, index, path);
	lua_pushnil(L);
	while (lua_next(L, index)) {
		const char *name = string(L, -2, path, false);
		bool found = false;
		for (size_t i = 0; names[i]; ++i)
			if (!strcmp(name, names[i])) found = true;
		if (!found)
			luaL_error(L, "%s.%s: unknown setting", path, name);
		lua_pop(L, 1);
	}
}

#define FIELDS(L, i, path, ...) \
	fields(L, i, path, (const char *const[]){ __VA_ARGS__, NULL })

static int
array(lua_State *L, int index, const char *path, int max)
{
	index = lua_absindex(L, index);
	table(L, index, path);
	size_t n = lua_rawlen(L, index), count = 0;
	if (n > (size_t)max)
		luaL_error(L, "%s: too many entries (maximum %d)", path, max);
	lua_pushnil(L);
	while (lua_next(L, index)) {
		lua_Number key = lua_tonumber(L, -2);
		if (lua_type(L, -2) != LUA_TNUMBER || !(key >= 1 && key <= n) ||
		    key != floor(key))
			luaL_error(L, "%s: expected a contiguous array starting at 1", path);
		++count;
		lua_pop(L, 1);
	}
	if (count != n)
		luaL_error(L, "%s: array contains a hole", path);
	return (int)n;
}

static bool
field(lua_State *L, int index, const char *name)
{
	lua_getfield(L, index, name);
	if (!lua_isnil(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

static int32_t
integer(lua_State *L, int index, const char *path, int32_t min, int32_t max)
{
	lua_Number n = lua_tonumber(L, index);
	if (lua_type(L, index) != LUA_TNUMBER || !(n >= min && n <= max) || n != floor(n))
		luaL_error(L, "%s: expected an integer from %d to %d", path, min, max);
	return (int32_t)n;
}

static bool
boolean(lua_State *L, int index, const char *path)
{
	if (!lua_isboolean(L, index))
		luaL_error(L, "%s: expected true or false", path);
	return lua_toboolean(L, index);
}

static int
one_of(lua_State *L, int index, const char *path, const char *const *choices)
{
	const char *value = string(L, index, path, false);
	for (int i = 0; choices[i]; ++i)
		if (!strcmp(value, choices[i]))
			return i;
	return luaL_error(L, "%s: invalid value '%s'", path, value);
}

static uint32_t
color(lua_State *L, int index, const char *path)
{
	const char *s = string(L, index, path, false);
	size_t n = strlen(s);
	uint32_t value = 0;
	if (*s != '#' || (n != 7 && n != 9))
		luaL_error(L, "%s: expected #RRGGBB or #AARRGGBB", path);
	for (++s; *s; ++s) {
		unsigned digit;
		if (*s >= '0' && *s <= '9') digit = *s - '0';
		else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
		else return luaL_error(L, "%s: invalid hexadecimal color", path);
		value = (value << 4) | digit;
	}
	return n == 7 ? value | 0xff000000 : value;
}

static void
copy_string(lua_State *L, char **out, int index, const char *path, bool empty)
{
	char *copy = strdup(string(L, index, path, empty));
	if (!copy)
		luaL_error(L, "%s: out of memory", path);
	free(*out);
	*out = copy;
}

static void
parse_argv(lua_State *L, char ***out, int index, const char *path)
{
	index = lua_absindex(L, index);
	int n = array(L, index, path, 256);
	if (!n)
		luaL_error(L, "%s: command needs a program name", path);
	*out = calloc(n + 1, sizeof(**out));
	if (!*out)
		luaL_error(L, "%s: out of memory", path);
	for (int i = 0; i < n; ++i) {
		char p[256];
		snprintf(p, sizeof(p), "%s[%d]", path, i + 1);
		lua_rawgeti(L, index, i + 1);
		copy_string(L, &(*out)[i], -1, p, i != 0);
		lua_pop(L, 1);
	}
}

/* --------------------------------------------------------------- layout */

static void
parse_layout(lua_State *L, struct config *cfg, int index)
{
	static const char *const modes[] = { "floating", "split", "quad", NULL };
	static const char *const axes[] = { "vertical", "horizontal", NULL };

	index = lua_absindex(L, index);
	FIELDS(L, index, "layout", "mode", "axis", "max");
	if (field(L, index, "mode")) {
		cfg->layout.mode = one_of(L, -1, "layout.mode", modes);
		lua_pop(L, 1);
	}
	if (field(L, index, "axis")) {
		cfg->layout.axis = one_of(L, -1, "layout.axis", axes);
		lua_pop(L, 1);
	}
	if (field(L, index, "max")) {
		cfg->layout.max = integer(L, -1, "layout.max", 1, 16);
		lua_pop(L, 1);
	}
	if (cfg->layout.mode == LAYOUT_QUAD && cfg->layout.max > 4)
		luaL_error(L, "layout.max: the quad layout holds at most 4 windows");
}

/* ------------------------------------------------------------- bindings */

static const struct command *
find_command(const char *name)
{
	for (int i = 0; i < cmd_last; ++i)
		if (commands[i].name && !strcmp(commands[i].name, name))
			return &commands[i];
	return NULL;
}

static bool
takes_text(enum cmd command)
{
	return command == cmd_layout || command == cmd_layout_axis;
}

static void
parse_binding(lua_State *L, struct config *cfg, int index, const char *path)
{
	index = lua_absindex(L, index);
	FIELDS(L, index, path, "key", "spawn", "action", "args", "window", "value");

	struct binding *b = calloc(1, sizeof(*b));
	if (!b)
		luaL_error(L, "%s: out of memory", path);
	wl_list_insert(cfg->bindings.prev, &b->link);

	lua_getfield(L, index, "key");
	const char *key = string(L, -1, path, false);
	if (!chara_parse_key(key, cfg->values.mod, &b->modifiers, &b->key, false))
		luaL_error(L, "%s.key: invalid key combination '%s'", path, key);
	lua_pop(L, 1);

	struct binding *other;
	wl_list_for_each(other, &cfg->bindings, link) {
		if (other == b) break;
		if (other->key == b->key && (other->modifiers == b->modifiers ||
		    other->modifiers == (uint32_t)SWC_MOD_ANY ||
		    b->modifiers == (uint32_t)SWC_MOD_ANY))
			luaL_error(L, "%s.key: duplicate or overlapping binding", path);
	}

	if (field(L, index, "spawn")) {
		parse_argv(L, &b->argv, -1, path);
		lua_pop(L, 1);
		if (field(L, index, "action") || field(L, index, "args") ||
		    field(L, index, "window") || field(L, index, "value"))
			luaL_error(L, "%s: spawn cannot be combined with an action", path);
		return;
	}

	lua_getfield(L, index, "action");
	const char *name = string(L, -1, path, false);
	const struct command *cmd = find_command(name);
	if (!cmd)
		luaL_error(L, "%s.action: unknown action '%s'", path, name);
	b->action.command = cmd->command;
	lua_pop(L, 1);

	if (field(L, index, "window")) {
		if (!cmd->selects)
			luaL_error(L, "%s.window: '%s' does not act on a window", path, name);
		copy_string(L, &b->action.selector, -1, path, false);
		lua_pop(L, 1);
	}

	if (takes_text(cmd->command)) {
		if (!field(L, index, "value"))
			luaL_error(L, "%s.value: '%s' requires %s", path, name, cmd->usage);
		copy_string(L, &b->action.text, -1, path, false);
		lua_pop(L, 1);
		return;
	}
	if (field(L, index, "value"))
		luaL_error(L, "%s.value: '%s' takes no string argument", path, name);

	b->action.argc = cmd->argc;
	if (!field(L, index, "args")) {
		if (cmd->argc)
			luaL_error(L, "%s.args: '%s' requires %s", path, name, cmd->usage);
		return;
	}
	if (!cmd->argc)
		luaL_error(L, "%s.args: '%s' takes no arguments", path, name);
	if (array(L, -1, path, 4) != cmd->argc)
		luaL_error(L, "%s.args: '%s' requires %s", path, name, cmd->usage);
	for (int i = 0; i < cmd->argc; ++i) {
		int32_t min = -32768, max = 32767;
		if (cmd->command == cmd_workspace || cmd->command == cmd_move_workspace)
			min = 1, max = CHARA_WORKSPACES;
		else if (cmd->command == cmd_layout_max)
			min = 1, max = 16;
		else if (cmd->command == cmd_resize_absolute ||
		         (cmd->command == cmd_teleport && i >= 2))
			min = 1, max = 32768;
		lua_rawgeti(L, -1, i + 1);
		b->action.args[i] = integer(L, -1, path, min, max);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

static bool
default_binding(struct config *cfg, const char *key, enum cmd command,
                int32_t argument, char *const *argv)
{
	struct binding *b = calloc(1, sizeof(*b));
	if (!b)
		return false;
	wl_list_insert(cfg->bindings.prev, &b->link);
	if (!chara_parse_key(key, cfg->values.mod, &b->modifiers, &b->key, false))
		return false;
	b->action.command = command;
	if (argument) {
		b->action.argc = 1;
		b->action.args[0] = argument;
	}
	return !argv || (b->argv = chara_argv_copy(argv));
}

static bool
default_bindings(struct config *cfg)
{
	if (!default_binding(cfg, "mod+Return", 0, 0, (char *[]){ "foot", NULL }) ||
	    !default_binding(cfg, "mod+q", cmd_close, 0, NULL) ||
	    !default_binding(cfg, "mod+f", cmd_maximize, 0, NULL) ||
	    !default_binding(cfg, "mod+shift+f", cmd_fullscreen, 0, NULL) ||
	    !default_binding(cfg, "mod+space", cmd_floating, 0, NULL) ||
	    !default_binding(cfg, "mod+m", cmd_minimize, 0, NULL) ||
	    !default_binding(cfg, "mod+n", cmd_restore, 0, NULL) ||
	    !default_binding(cfg, "mod+c", cmd_center, 0, NULL) ||
	    !default_binding(cfg, "mod+j", cmd_focus_next, 0, NULL) ||
	    !default_binding(cfg, "mod+k", cmd_focus_prev, 0, NULL) ||
	    !default_binding(cfg, "mod+shift+r", cmd_reload, 0, NULL) ||
	    !default_binding(cfg, "mod+shift+e", cmd_quit, 0, NULL))
		return false;
	for (int i = 1; i <= CHARA_WORKSPACES; ++i) {
		char key[32];
		snprintf(key, sizeof(key), "mod+%d", i);
		if (!default_binding(cfg, key, cmd_workspace, i, NULL))
			return false;
		snprintf(key, sizeof(key), "mod+shift+%d", i);
		if (!default_binding(cfg, key, cmd_move_workspace, i, NULL))
			return false;
	}
	return true;
}

/* ---------------------------------------------------------------- rules */

static void
parse_rules(lua_State *L, struct config *cfg, int index)
{
	index = lua_absindex(L, index);
	int n = array(L, index, "rules", 4096);

	for (int i = 1; i <= n; ++i) {
		char path[64];
		snprintf(path, sizeof(path), "rules[%d]", i);
		lua_rawgeti(L, index, i);
		int t = lua_gettop(L);
		FIELDS(L, t, path, "app_id", "id", "width", "height", "x", "y",
		       "center", "titlebar", "floating", "movable", "resizable");

		struct rule *r = calloc(1, sizeof(*r));
		if (!r)
			luaL_error(L, "%s: out of memory", path);
		r->movable = true;
		r->resizable = true;
		wl_list_insert(cfg->rules.prev, &r->link);

		lua_getfield(L, t, "app_id");
		const char *app = string(L, -1, path, false);
		if (strlen(app) >= sizeof(r->app_id))
			luaL_error(L, "%s.app_id: maximum %d bytes", path,
			           (int)sizeof(r->app_id) - 1);
		strcpy(r->app_id, app);
		lua_pop(L, 1);

		struct rule *other;
		wl_list_for_each(other, &cfg->rules, link) {
			if (other == r) break;
			if (!strcmp(other->app_id, r->app_id))
				luaL_error(L, "%s.app_id: duplicate rule", path);
		}

		if (field(L, t, "id")) {
			const char *id = string(L, -1, path, false);
			if (strlen(id) >= sizeof(r->name))
				luaL_error(L, "%s.id: maximum %d bytes", path,
				           (int)sizeof(r->name) - 1);
			if (*id == '#' || strchr(id, ':'))
				luaL_error(L, "%s.id: '#' and ':' are reserved", path);
			strcpy(r->name, id);
			lua_pop(L, 1);
		}
		if (field(L, t, "width")) { r->width = integer(L, -1, path, 1, 32768); lua_pop(L, 1); }
		if (field(L, t, "height")) { r->height = integer(L, -1, path, 1, 32768); lua_pop(L, 1); }
		if (!!r->width != !!r->height)
			luaL_error(L, "%s: width and height must be set together", path);

		bool x = false, y = false;
		if (field(L, t, "x")) { x = true; r->x = integer(L, -1, path, -32768, 32767); lua_pop(L, 1); }
		if (field(L, t, "y")) { y = true; r->y = integer(L, -1, path, -32768, 32767); lua_pop(L, 1); }
		if (x != y)
			luaL_error(L, "%s: x and y must be set together", path);
		r->has_pos = x;

		if (field(L, t, "titlebar")) {
			r->has_titlebar = true;
			r->titlebar = boolean(L, -1, path);
			lua_pop(L, 1);
		}
		if (field(L, t, "floating")) {
			r->has_floating = true;
			r->floating = boolean(L, -1, path);
			lua_pop(L, 1);
		}
		if (field(L, t, "movable")) { r->movable = boolean(L, -1, path); lua_pop(L, 1); }
		if (field(L, t, "resizable")) { r->resizable = boolean(L, -1, path); lua_pop(L, 1); }
		if (field(L, t, "center")) { r->center = boolean(L, -1, path); lua_pop(L, 1); }
		if (x && r->center)
			luaL_error(L, "%s: center conflicts with x and y", path);
		lua_pop(L, 1);
	}
}

/* --------------------------------------------------------------- startup */

static void
parse_commands(lua_State *L, struct wl_list *list, int index, const char *path)
{
	index = lua_absindex(L, index);
	int n = array(L, index, path, 4096);

	for (int i = 1; i <= n; ++i) {
		char p[128];
		snprintf(p, sizeof(p), "%s[%d]", path, i);
		struct startup_command *c = calloc(1, sizeof(*c));
		if (!c)
			luaL_error(L, "%s: out of memory", p);
		wl_list_insert(list->prev, &c->link);
		lua_rawgeti(L, index, i);
		int t = lua_gettop(L);
		c->timeout_ms = 10000;

		if (field(L, t, "argv")) {
			parse_argv(L, &c->argv, -1, p);
			lua_pop(L, 1);
			if (strcmp(path, "exec_once"))
				luaL_error(L, "%s: startup controls are only supported in exec_once", p);
			FIELDS(L, t, p, "argv", "wait", "ready_socket", "stop_on_exit", "timeout_ms");
			if (field(L, t, "wait")) { c->wait = boolean(L, -1, p); lua_pop(L, 1); }
			if (field(L, t, "stop_on_exit")) { c->stop_on_exit = boolean(L, -1, p); lua_pop(L, 1); }
			if (field(L, t, "ready_socket")) { copy_string(L, &c->ready_socket, -1, p, false); lua_pop(L, 1); }
			if (field(L, t, "timeout_ms")) {
				c->timeout_ms = integer(L, -1, p, 100, 60000);
				lua_pop(L, 1);
				if (!c->wait && !c->ready_socket)
					luaL_error(L, "%s: timeout_ms requires wait or ready_socket", p);
			}
			if (c->wait && c->ready_socket)
				luaL_error(L, "%s: choose wait or ready_socket, not both", p);
			if (c->ready_socket && !c->stop_on_exit)
				luaL_error(L, "%s: ready_socket requires stop_on_exit", p);
		} else {
			parse_argv(L, &c->argv, t, p);
		}
		lua_pop(L, 1);
	}
}

static void
parse_monitors(lua_State *L, struct config *cfg, int index)
{
	index = lua_absindex(L, index);
	int n = array(L, index, "monitors", 32);

	for (int i = 1; i <= n; ++i) {
		char path[64], member[80];
		snprintf(path, sizeof(path), "monitors[%d]", i);
		lua_rawgeti(L, index, i);
		int t = lua_gettop(L);
		FIELDS(L, t, path, "name", "x", "y");

		struct monitor_config *m = calloc(1, sizeof(*m));
		if (!m)
			luaL_error(L, "%s: out of memory", path);
		wl_list_insert(cfg->monitors.prev, &m->link);

		lua_getfield(L, t, "name");
		snprintf(member, sizeof(member), "%s.name", path);
		copy_string(L, &m->name, -1, member, false);
		lua_pop(L, 1);

		struct monitor_config *other;
		wl_list_for_each(other, &cfg->monitors, link) {
			if (other == m) break;
			if (!strcmp(other->name, m->name))
				luaL_error(L, "%s.name: duplicate monitor", path);
		}
		lua_getfield(L, t, "x");
		snprintf(member, sizeof(member), "%s.x", path);
		m->x = integer(L, -1, member, -32768, 32767);
		lua_pop(L, 1);
		lua_getfield(L, t, "y");
		snprintf(member, sizeof(member), "%s.y", path);
		m->y = integer(L, -1, member, -32768, 32767);
		lua_pop(L, 2);
	}
}

/* ----------------------------------------------------------- appearance */

static void
parse_rings(lua_State *L, struct config *cfg, int index)
{
	index = lua_absindex(L, index);
	int n = array(L, index, "appearance.rings", CHARA_MAX_RINGS);

	cfg->values.ring_count = (unsigned)n;
	for (int i = 1; i <= n; ++i) {
		char path[64];
		snprintf(path, sizeof(path), "appearance.rings[%d]", i);
		lua_rawgeti(L, index, i);
		int t = lua_gettop(L);
		FIELDS(L, t, path, "width", "focused", "unfocused");

		struct ring *ring = &cfg->values.rings[i - 1];
		*ring = (struct ring){ 1, 0xfffabd2f, 0xff3c3836 };
		if (field(L, t, "width")) { ring->width = integer(L, -1, path, 0, 64); lua_pop(L, 1); }
		if (field(L, t, "focused")) { ring->focused = color(L, -1, path); lua_pop(L, 1); }
		if (field(L, t, "unfocused")) { ring->unfocused = color(L, -1, path); lua_pop(L, 1); }
		lua_pop(L, 1);
	}
}

static void
parse_title(lua_State *L, struct config *cfg, int index)
{
	static const char *const edges[] = { "top", "right", "bottom", "left", NULL };
	static const char *const aligns[] = { "start", "center", "end", NULL };
	struct decor *d = cfg->decoration;

	index = lua_absindex(L, index);
	FIELDS(L, index, "appearance.title", "enabled", "font", "format", "edge",
	       "align", "foreground", "background", "padding", "offset_x", "offset_y");

	if (field(L, index, "enabled")) { d->enabled = boolean(L, -1, "appearance.title.enabled"); lua_pop(L, 1); }
	if (field(L, index, "foreground")) { d->foreground = color(L, -1, "appearance.title.foreground"); lua_pop(L, 1); }
	if (field(L, index, "background")) { d->background = color(L, -1, "appearance.title.background"); lua_pop(L, 1); }
	if (field(L, index, "padding")) { d->padding = integer(L, -1, "appearance.title.padding", 0, 256); lua_pop(L, 1); }
	if (field(L, index, "offset_x")) { d->offset_x = integer(L, -1, "appearance.title.offset_x", -4096, 4096); lua_pop(L, 1); }
	if (field(L, index, "offset_y")) { d->offset_y = integer(L, -1, "appearance.title.offset_y", -4096, 4096); lua_pop(L, 1); }
	if (field(L, index, "font")) { copy_string(L, &d->fontname, -1, "appearance.title.font", false); lua_pop(L, 1); }
	if (field(L, index, "format")) { copy_string(L, &cfg->values.title_format, -1, "appearance.title.format", true); lua_pop(L, 1); }
	if (field(L, index, "edge")) { d->edge = one_of(L, -1, "appearance.title.edge", edges); lua_pop(L, 1); }
	if (field(L, index, "align")) { d->align = one_of(L, -1, "appearance.title.align", aligns); lua_pop(L, 1); }
}

static void
parse_titlebar(lua_State *L, struct config *cfg, int index)
{
	static const char *const styles[] = { "classic", "circles", NULL };
	static const char *const sides[] = { "left", "right", NULL };
	static const char *const positions[] = { "left", "center", "right", NULL };
	static const char *const fullscreen[] = { "fullscreen", "maximize", NULL };
	struct decor *d = cfg->decoration;

	index = lua_absindex(L, index);
	FIELDS(L, index, "appearance.titlebar", "enabled", "height", "padding",
	       "buttons", "buttons_style", "buttons_position", "title_position",
	       "circle_colors", "fullscreen_action", "focused", "unfocused");

	if (field(L, index, "enabled")) { d->titlebar.enabled = boolean(L, -1, "appearance.titlebar.enabled"); lua_pop(L, 1); }
	if (field(L, index, "height")) { d->bar_height = integer(L, -1, "appearance.titlebar.height", 16, 128); lua_pop(L, 1); }
	if (field(L, index, "padding")) { d->bar_padding = integer(L, -1, "appearance.titlebar.padding", 0, 64); lua_pop(L, 1); }
	if (field(L, index, "buttons_style")) {
		d->titlebar.buttons_style = one_of(L, -1, "appearance.titlebar.buttons_style", styles);
		lua_pop(L, 1);
	}
	if (field(L, index, "buttons_position")) {
		d->titlebar.buttons_left = one_of(L, -1, "appearance.titlebar.buttons_position", sides) == 0;
		lua_pop(L, 1);
	}
	if (field(L, index, "title_position")) {
		d->bar_align = one_of(L, -1, "appearance.titlebar.title_position", positions);
		d->bar_align_set = true;
		lua_pop(L, 1);
	}
	if (field(L, index, "fullscreen_action")) {
		d->fullscreen_action = one_of(L, -1, "appearance.titlebar.fullscreen_action", fullscreen);
		lua_pop(L, 1);
	}
	if (field(L, index, "circle_colors")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "appearance.titlebar.circle_colors", "preset", "close",
		       "minimize", "fullscreen");
		if (field(L, t, "preset")) {
			const char *s = string(L, -1, "appearance.titlebar.circle_colors.preset", false);
			if (strcmp(s, "macos"))
				luaL_error(L, "appearance.titlebar.circle_colors.preset: expected macos");
			d->titlebar.close_color = 0xffff5f57;
			d->titlebar.minimize_color = 0xffffbd2e;
			d->titlebar.fullscreen_color = 0xff28c840;
			lua_pop(L, 1);
		}
		if (field(L, t, "close")) { d->titlebar.close_color = color(L, -1, "appearance.titlebar.circle_colors.close") | 0xff000000; lua_pop(L, 1); }
		if (field(L, t, "minimize")) { d->titlebar.minimize_color = color(L, -1, "appearance.titlebar.circle_colors.minimize") | 0xff000000; lua_pop(L, 1); }
		if (field(L, t, "fullscreen")) { d->titlebar.fullscreen_color = color(L, -1, "appearance.titlebar.circle_colors.fullscreen") | 0xff000000; lua_pop(L, 1); }
		lua_pop(L, 1);
	}
	if (field(L, index, "buttons")) {
		static const char *const names[] = { "minimize", "fullscreen", "close", NULL };
		static const enum swc_titlebar_action values[] = {
			SWC_TITLEBAR_MINIMIZE, SWC_TITLEBAR_FULLSCREEN, SWC_TITLEBAR_CLOSE
		};
		int t = lua_gettop(L);
		d->titlebar.count = array(L, t, "appearance.titlebar.buttons", 3);
		memset(d->titlebar.buttons, 0, sizeof(d->titlebar.buttons));
		for (unsigned i = 0; i < d->titlebar.count; ++i) {
			lua_rawgeti(L, t, i + 1);
			enum swc_titlebar_action a =
			    values[one_of(L, -1, "appearance.titlebar.buttons", names)];
			for (unsigned j = 0; j < i; ++j)
				if (d->titlebar.buttons[j] == a)
					luaL_error(L, "appearance.titlebar.buttons: duplicate button");
			d->titlebar.buttons[i] = a;
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}

	const char *states[] = { "focused", "unfocused" };
	for (int i = 0; i < 2; ++i) {
		if (!field(L, index, states[i]))
			continue;
		int t = lua_gettop(L);
		char path[96];
		snprintf(path, sizeof(path), "appearance.titlebar.%s", states[i]);
		bool classic = d->titlebar.buttons_style == SWC_TITLEBAR_BUTTONS_CLASSIC;
		if (i || !classic) {
			const char *feedback[] = { "hover", "pressed" };
			for (size_t j = 0; j < 2; ++j)
				if (field(L, t, feedback[j]))
					luaL_error(L, "%s.%s: only supported in focused with buttons_style = 'classic'",
					           path, feedback[j]);
			FIELDS(L, t, path, "background", "foreground");
		} else {
			FIELDS(L, t, path, "background", "foreground", "hover", "pressed");
		}
		struct titlebar_style *style = i ? &d->bar_unfocused : &d->bar_focused;
		if (field(L, t, "background")) { style->background = color(L, -1, path) | 0xff000000; lua_pop(L, 1); }
		if (field(L, t, "foreground")) { style->foreground = color(L, -1, path) | 0xff000000; lua_pop(L, 1); }
		if (!i && classic) {
			if (field(L, t, "hover")) { d->bar_hover = color(L, -1, path) | 0xff000000; lua_pop(L, 1); }
			if (field(L, t, "pressed")) { d->bar_pressed = color(L, -1, path) | 0xff000000; lua_pop(L, 1); }
		}
		lua_pop(L, 1);
	}
}

static void
parse_wallpaper(lua_State *L, struct config *cfg, int index, const char *filename)
{
	static const char *const modes[] = { "fill", "fit", "center", NULL };

	index = lua_absindex(L, index);
	FIELDS(L, index, "appearance.wallpaper", "path", "mode", "background");

	if (field(L, index, "mode")) {
		cfg->wallpaper.mode = one_of(L, -1, "appearance.wallpaper.mode", modes);
		lua_pop(L, 1);
	}
	if (field(L, index, "background")) {
		uint32_t value = color(L, -1, "appearance.wallpaper.background");
		if ((value >> 24) != 255)
			luaL_error(L, "appearance.wallpaper.background: must be opaque");
		cfg->wallpaper.background = value;
		lua_pop(L, 1);
	}
	if (field(L, index, "path")) {
		const char *path = string(L, -1, "appearance.wallpaper.path", false);
		/* Keep the expanded path in Lua memory across any parser error. */
		if (!strncmp(path, "~/", 2)) {
			const char *home = getenv("HOME");
			if (!home || *home != '/')
				luaL_error(L, "appearance.wallpaper.path: ~/ requires an absolute HOME");
			lua_pushstring(L, home);
			lua_pushstring(L, path + 1);
			lua_concat(L, 2);
		} else if (*path != '/') {
			if (*path == '~')
				luaL_error(L, "appearance.wallpaper.path: only ~/ home expansion is supported");
			const char *slash = strrchr(filename, '/');
			lua_pushlstring(L, filename, slash ? (size_t)(slash - filename + 1) : 0);
			lua_pushstring(L, path);
			lua_concat(L, 2);
		} else {
			lua_pushvalue(L, -1);
		}
		copy_string(L, &cfg->wallpaper.path, -1, "appearance.wallpaper.path", false);
		lua_pop(L, 2);
	}
}

static void
parse_appearance(lua_State *L, struct config *cfg, int index, const char *filename)
{
	index = lua_absindex(L, index);
	FIELDS(L, index, "appearance", "rings", "title", "titlebar", "cursor",
	       "font", "wallpaper", "maximize");

	if (field(L, index, "maximize")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "appearance.maximize", "borders", "titlebar");
		if (field(L, t, "borders")) {
			cfg->values.maximize_borders = boolean(L, -1, "appearance.maximize.borders");
			lua_pop(L, 1);
		}
		if (field(L, t, "titlebar")) {
			cfg->values.maximize_titlebar = boolean(L, -1, "appearance.maximize.titlebar");
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (field(L, index, "rings")) { parse_rings(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, index, "wallpaper")) { parse_wallpaper(L, cfg, -1, filename); lua_pop(L, 1); }
	if (field(L, index, "font")) {
		copy_string(L, &cfg->decoration->fontname, -1, "appearance.font", false);
		lua_pop(L, 1);
	}
	if (field(L, index, "title")) { parse_title(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, index, "titlebar")) { parse_titlebar(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, index, "cursor")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "appearance.cursor", "theme", "size");
		if (field(L, t, "theme")) { copy_string(L, &cfg->cursor_theme, -1, "appearance.cursor.theme", false); lua_pop(L, 1); }
		if (field(L, t, "size")) { cfg->cursor_size = integer(L, -1, "appearance.cursor.size", 1, 256); lua_pop(L, 1); }
		lua_pop(L, 1);
	}
}

/* ------------------------------------------------------------------ bar */

/* charabar reads this section itself. charaWC validates it so a mistake is
 * reported once, at the same place as every other setting. */

static void
bar_string(lua_State *L, int index, const char *name, const char *path, bool empty)
{
	if (field(L, index, name)) {
		string(L, -1, path, empty);
		lua_pop(L, 1);
	}
}

static void
parse_bar(lua_State *L, struct config *cfg, int index)
{
	static const char *const positions[] = { "top", "bottom", NULL };
	static const char *const layers[] = { "top", "bottom", NULL };
	static const char *const module_names[] = {
		"workspaces", "window", "taskbar", "clock", "cpu", "memory",
		"network", "volume", NULL
	};
	static const char *const scopes[] = { "workspace", "monitor", "all", NULL };
	static const char *const overflows[] = { "shrink", "scroll", "none", NULL };

	index = lua_absindex(L, index);
	FIELDS(L, index, "bar", "enabled", "position", "layer", "height",
	       "exclusive", "background", "foreground", "accent", "muted",
	       "font", "padding", "spacing", "modules", "workspaces", "window",
	       "taskbar", "clock", "cpu", "memory", "network", "volume");

	if (field(L, index, "enabled")) { cfg->bar.enabled = boolean(L, -1, "bar.enabled"); lua_pop(L, 1); }
	if (field(L, index, "position")) { one_of(L, -1, "bar.position", positions); lua_pop(L, 1); }
	if (field(L, index, "layer")) { one_of(L, -1, "bar.layer", layers); lua_pop(L, 1); }
	if (field(L, index, "height")) { integer(L, -1, "bar.height", 16, 128); lua_pop(L, 1); }
	if (field(L, index, "exclusive")) { boolean(L, -1, "bar.exclusive"); lua_pop(L, 1); }

	const char *colors[] = { "background", "foreground", "accent", "muted" };
	for (size_t i = 0; i < sizeof(colors) / sizeof(*colors); ++i) {
		if (field(L, index, colors[i])) {
			color(L, -1, "bar color");
			lua_pop(L, 1);
		}
	}
	bar_string(L, index, "font", "bar.font", false);
	if (field(L, index, "padding")) { integer(L, -1, "bar.padding", 0, 128); lua_pop(L, 1); }
	if (field(L, index, "spacing")) { integer(L, -1, "bar.spacing", 0, 128); lua_pop(L, 1); }

	if (field(L, index, "modules")) {
		int modules = lua_gettop(L);
		FIELDS(L, modules, "bar.modules", "left", "center", "right");
		const char *sections[] = { "left", "center", "right" };
		for (size_t section = 0; section < 3; ++section) {
			if (!field(L, modules, sections[section]))
				continue;
			int n = array(L, -1, "bar.modules section", 16);
			for (int i = 1; i <= n; ++i) {
				lua_rawgeti(L, -1, i);
				one_of(L, -1, "bar.modules entry", module_names);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (field(L, index, "workspaces")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "bar.workspaces", "count", "format");
		if (field(L, t, "count")) { integer(L, -1, "bar.workspaces.count", 1, CHARA_WORKSPACES); lua_pop(L, 1); }
		bar_string(L, t, "format", "bar.workspaces.format", false);
		lua_pop(L, 1);
	}
	if (field(L, index, "window")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "bar.window", "empty", "max_length");
		bar_string(L, t, "empty", "bar.window.empty", true);
		if (field(L, t, "max_length")) { integer(L, -1, "bar.window.max_length", 1, 512); lua_pop(L, 1); }
		lua_pop(L, 1);
	}
	if (field(L, index, "taskbar")) {
		int t = lua_gettop(L);
		FIELDS(L, t, "bar.taskbar", "max_length", "scope", "overflow",
		       "min_width", "max_width", "scroll_step");
		if (field(L, t, "max_length")) { integer(L, -1, "bar.taskbar.max_length", 1, 128); lua_pop(L, 1); }
		if (field(L, t, "scope")) { one_of(L, -1, "bar.taskbar.scope", scopes); lua_pop(L, 1); }
		if (field(L, t, "overflow")) { one_of(L, -1, "bar.taskbar.overflow", overflows); lua_pop(L, 1); }
		if (field(L, t, "min_width")) { integer(L, -1, "bar.taskbar.min_width", 16, 512); lua_pop(L, 1); }
		if (field(L, t, "max_width")) { integer(L, -1, "bar.taskbar.max_width", 0, 16384); lua_pop(L, 1); }
		if (field(L, t, "scroll_step")) { integer(L, -1, "bar.taskbar.scroll_step", 1, 1024); lua_pop(L, 1); }
		lua_pop(L, 1);
	}

	const char *timed[] = { "clock", "cpu", "memory", "network", "volume" };
	for (size_t i = 0; i < sizeof(timed) / sizeof(*timed); ++i) {
		if (!field(L, index, timed[i]))
			continue;
		int t = lua_gettop(L);
		if (!strcmp(timed[i], "network")) {
			FIELDS(L, t, "bar.network", "format_online", "format_offline", "interval");
			bar_string(L, t, "format_online", "bar.network.format_online", false);
			bar_string(L, t, "format_offline", "bar.network.format_offline", true);
		} else if (!strcmp(timed[i], "volume")) {
			FIELDS(L, t, "bar.volume", "format", "format_muted", "interval");
			bar_string(L, t, "format", "bar.volume.format", false);
			bar_string(L, t, "format_muted", "bar.volume.format_muted", true);
		} else {
			FIELDS(L, t, "bar timed module", "format", "interval");
			bar_string(L, t, "format", "bar module format", false);
		}
		/* The volume module also refreshes on SIGUSR1, so 0 means "never
		 * poll". The others have no such trigger and would simply freeze. */
		if (field(L, t, "interval")) {
			integer(L, -1, "bar module interval",
			        strcmp(timed[i], "volume") ? 1 : 0, 3600);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
}

/* ----------------------------------------------------------------- load */

static int
config_getenv(lua_State *L)
{
	const char *value = getenv(string(L, 1, "os.getenv", false));
	if (value)
		lua_pushstring(L, value);
	else
		lua_pushnil(L);
	return 1;
}

struct config_source {
	const char *data;
	size_t length;
};

static int
parse(lua_State *L)
{
	struct config *cfg = lua_touserdata(L, 1);
	const char *filename = lua_tostring(L, 2);
	const struct { const char *name; lua_CFunction open; } libs[] = {
		{ "_G", luaopen_base }, { "table", luaopen_table },
		{ "string", luaopen_string }, { "math", luaopen_math },
	};

	for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); ++i) {
		luaL_requiref(L, libs[i].name, libs[i].open, 1);
		lua_pop(L, 1);
	}
	/* A declarative config may read the environment. Programs are launched
	 * from exec/exec_once, never during parsing: no io, loader or execute. */
	lua_pushnil(L); lua_setglobal(L, "dofile");
	lua_pushnil(L); lua_setglobal(L, "loadfile");
	lua_pushnil(L); lua_setglobal(L, "load");
	lua_newtable(L);
	lua_pushcfunction(L, config_getenv);
	lua_setfield(L, -2, "getenv");
	lua_setglobal(L, "os");

	const struct config_source *source = lua_touserdata(L, 3);
	lua_pushfstring(L, "@%s", filename);
	if (luaL_loadbufferx(L, source->data, source->length, lua_tostring(L, -1), "t") != LUA_OK)
		return lua_error(L);
	lua_call(L, 0, 1);

	int root = lua_gettop(L);
	FIELDS(L, root, "config", "mod", "raise_maximized_on_click", "layout",
	       "appearance", "bar", "bindings", "rules", "exec_once", "exec",
	       "monitors");

	if (field(L, root, "mod")) {
		uint32_t key;
		if (!chara_parse_key(string(L, -1, "mod", false), 0, &cfg->values.mod, &key, true))
			luaL_error(L, "mod: expected modifier names such as logo or ctrl+alt");
		lua_pop(L, 1);
	}
	if (field(L, root, "raise_maximized_on_click")) {
		cfg->values.raise_maximized_on_click = boolean(L, -1, "raise_maximized_on_click");
		lua_pop(L, 1);
	}
	if (field(L, root, "layout")) { parse_layout(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, root, "appearance")) { parse_appearance(L, cfg, -1, filename); lua_pop(L, 1); }
	if (field(L, root, "bar")) { parse_bar(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, root, "bindings")) {
		int t = lua_gettop(L), n = array(L, t, "bindings", 4096);
		for (int i = 1; i <= n; ++i) {
			char path[64];
			snprintf(path, sizeof(path), "bindings[%d]", i);
			lua_rawgeti(L, t, i);
			parse_binding(L, cfg, -1, path);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	} else if (!default_bindings(cfg)) {
		luaL_error(L, "bindings: out of memory");
	}
	if (field(L, root, "rules")) { parse_rules(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, root, "monitors")) { parse_monitors(L, cfg, -1); lua_pop(L, 1); }
	if (field(L, root, "exec_once")) { parse_commands(L, &cfg->exec_once, -1, "exec_once"); lua_pop(L, 1); }
	if (field(L, root, "exec")) { parse_commands(L, &cfg->exec, -1, "exec"); lua_pop(L, 1); }
	return 0;
}

static int
traceback(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	luaL_traceback(L, L, msg ? msg : "configuration raised a non-string error", 1);
	return 1;
}

bool
chara_config_init(struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	wl_list_init(&cfg->bindings);
	wl_list_init(&cfg->rules);
	wl_list_init(&cfg->exec_once);
	wl_list_init(&cfg->exec);
	wl_list_init(&cfg->monitors);

	cfg->wallpaper.background = 0xff1d2021;
	cfg->wallpaper.mode = SWC_WALLPAPER_FILL;
	cfg->layout = (struct layout){ LAYOUT_FLOATING, SPLIT_VERTICAL, 4 };
	cfg->values = (struct values){
		.mod = SWC_MOD_LOGO,
		/* Keep the frame on screen when a window is maximized. */
		.maximize_borders = true,
		.maximize_titlebar = true,
		.ring_count = 1,
		.rings = {
			{ .width = 2, .focused = 0xfffabd2f, .unfocused = 0xff3c3836 },
		},
		.title_format = strdup("%t"),
	};
	cfg->decoration = decor_create();
	return cfg->values.title_format && cfg->decoration;
}

struct lua_budget {
	size_t bytes;
	unsigned instructions;
	struct timespec start;
};

static void *
config_alloc(void *data, void *ptr, size_t old_size, size_t size)
{
	struct lua_budget *budget = data;

	if (!ptr)
		old_size = 0; /* Lua uses old_size as a type tag for new objects. */
	if (!size) {
		free(ptr);
		budget->bytes -= old_size;
		return NULL;
	}
	if (size > 64 * 1024 * 1024 - (budget->bytes - old_size))
		return NULL;
	void *next = realloc(ptr, size);
	if (next)
		budget->bytes = budget->bytes - old_size + size;
	return next;
}

static void
instruction_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	void *data;
	lua_getallocf(L, &data);
	struct lua_budget *budget = data;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t elapsed = (now.tv_sec - budget->start.tv_sec) * INT64_C(1000000000) +
	                  now.tv_nsec - budget->start.tv_nsec;
	budget->instructions += 1000;
	if (budget->instructions >= 1000000 || elapsed > 250000000)
		luaL_error(L, "configuration exceeded its execution limit");
}

bool
chara_config_load(struct config *cfg, const char *path)
{
	if (!path)
		return default_bindings(cfg);

	/* Snapshot a regular file; never block on a FIFO or parse a partial write. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	struct stat st;
	if (fd < 0) {
		_wrn("%s: %s", path, strerror(errno));
		return false;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    st.st_size > 4 * 1024 * 1024) {
		_wrn("%s: expected a regular config file of at most 4 MiB", path);
		close(fd);
		return false;
	}
	size_t length = (size_t)st.st_size;
	char *source = malloc(length + 1);
	if (!source) {
		close(fd);
		return false;
	}
	size_t used = 0;
	while (used < length) {
		ssize_t n = read(fd, source + used, length - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		used += (size_t)n;
	}
	struct stat after;
	bool stable = fstat(fd, &after) == 0 && after.st_size == st.st_size &&
	              after.st_mtim.tv_sec == st.st_mtim.tv_sec &&
	              after.st_mtim.tv_nsec == st.st_mtim.tv_nsec;
	close(fd);
	if (used != length || !stable) {
		_wrn("%s: file changed while reading; try again after saving", path);
		free(source);
		return false;
	}

	struct lua_budget budget = {0};
	clock_gettime(CLOCK_MONOTONIC, &budget.start);
	lua_State *L = lua_newstate(config_alloc, &budget);
	if (!L) {
		free(source);
		_wrn("%s: couldn't create a Lua state", path);
		return false;
	}
	lua_sethook(L, instruction_hook, LUA_MASKCOUNT, 1000);
	lua_pushcfunction(L, traceback);
	lua_pushcfunction(L, parse);
	lua_pushlightuserdata(L, cfg);
	lua_pushstring(L, path);
	struct config_source snapshot = { source, length };
	lua_pushlightuserdata(L, &snapshot);
	bool ok = lua_pcall(L, 3, 0, 1) == LUA_OK;
	free(source);
	if (!ok)
		_wrn("%s: %s", path, lua_tostring(L, -1));
	lua_close(L);

	/* Image decoding has its own limits and sits outside the Lua budget, so
	 * both checking and reloading validate the real PNG here. */
	if (ok && cfg->wallpaper.path)
		ok = chara_wallpaper_load(&cfg->wallpaper, cfg->wallpaper.path);
	return ok;
}

void
chara_config_finish(struct config *cfg)
{
	struct binding *b, *bt;
	wl_list_for_each_safe(b, bt, &cfg->bindings, link) {
		wl_list_remove(&b->link);
		chara_binding_free(b);
	}
	struct rule *r, *rt;
	wl_list_for_each_safe(r, rt, &cfg->rules, link) {
		wl_list_remove(&r->link);
		free(r);
	}
	struct monitor_config *m, *mt;
	wl_list_for_each_safe(m, mt, &cfg->monitors, link) {
		wl_list_remove(&m->link);
		free(m->name);
		free(m);
	}
	struct wl_list *lists[] = { &cfg->exec_once, &cfg->exec };
	for (size_t i = 0; i < 2; ++i) {
		struct startup_command *c, *ct;
		wl_list_for_each_safe(c, ct, lists[i], link) {
			wl_list_remove(&c->link);
			chara_startup_command_free(c);
		}
	}
	free(cfg->values.title_format);
	free(cfg->cursor_theme);
	free(cfg->wallpaper.path);
	free(cfg->wallpaper.pixels);
	decor_destroy(cfg->decoration);
}

void
chara_config_move(struct config *dst, struct config *src)
{
	struct wl_list *to[] = { &dst->bindings, &dst->rules, &dst->exec_once,
	                         &dst->exec, &dst->monitors };
	struct wl_list *from[] = { &src->bindings, &src->rules, &src->exec_once,
	                           &src->exec, &src->monitors };

	/* The scalar members move by assignment; the list heads must not, since
	 * every element's prev/next points back at the head it was inserted on.
	 * Copying one would leave the elements addressing src's head - which for
	 * an empty list is the head itself, so a later walk of dst would hand
	 * container_of() an interior pointer of src and free() would abort. */
	*dst = *src;
	for (size_t i = 0; i < sizeof(to) / sizeof(*to); ++i) {
		wl_list_init(to[i]);
		wl_list_insert_list(to[i], from[i]); /* re-points elements at dst */
		wl_list_init(from[i]);
	}
}

bool
chara_config_bindings(struct config *cfg)
{
	struct binding *b, *tmp;

	wl_list_for_each_safe(b, tmp, &cfg->bindings, link) {
		wl_list_remove(&b->link);
		if (!chara_binding_install(b)) {
			wl_list_insert(&cfg->bindings, &b->link);
			return false;
		}
	}
	return true;
}

void
chara_config_start(struct config *cfg)
{
	chara_startup_run(cfg);
}

void
chara_startup_command_free(struct startup_command *c)
{
	chara_argv_free(c->argv);
	free(c->ready_socket);
	free(c);
}

/* The configuration charaWC writes when it finds none. It is deliberately
 * short: the settings not named here keep their built-in values, which
 * CONFIG.md documents in full. */
static const char config_example[] =
	"-- charaWC configuration. See CONFIG.md for every setting.\n"
	"--\n"
	"-- charaWC wrote this file because it found none of its own. Edit it freely:\n"
	"-- Super+Shift+R reloads it in place, and if an edit does not parse, charaWC\n"
	"-- says so in the log and keeps running the configuration it already has.\n"
	"\n"
	"return {\n"
	"	mod = \"logo\", -- Super/Windows key; also \"alt\" or \"ctrl+alt\"\n"
	"\n"
	"	layout = {\n"
	"		mode = \"split\",    -- \"floating\", \"split\" or \"quad\"\n"
	"		axis = \"vertical\", -- \"vertical\" side by side, \"horizontal\" stacked\n"
	"		max = 4,           -- windows past this open floating\n"
	"	},\n"
	"\n"
	"	appearance = {\n"
	"		wallpaper = { background = \"#282828\" },\n"
	"		titlebar = { enabled = true, height = 28 },\n"
	"		rings = {\n"
	"			{ width = 2, focused = \"#fabd2f\", unfocused = \"#3c3836\" },\n"
	"		},\n"
	"	},\n"
	"\n"
	"	bar = {\n"
	"		enabled = true,\n"
	"		position = \"top\",\n"
	"		height = 30,\n"
	"		modules = {\n"
	"			left = { \"workspaces\" },\n"
	"			center = { \"taskbar\" },\n"
	"			right = { \"memory\", \"clock\" },\n"
	"		},\n"
	"		-- Formats are strftime: %H:%M is a 24-hour clock, %I:%M %p a 12-hour\n"
	"		-- one. %H is the hour and %M the minute; %m and %h are months.\n"
	"		clock = { format = \"%a %d/%m/%Y  %H:%M\", interval = 30 },\n"
	"		memory = { format = \"RAM %p%\", interval = 2 },\n"
	"	},\n"
	"\n"
	"	bindings = {\n"
	"		{ key = \"mod+Return\", spawn = { \"foot\" } },\n"
	"		{ key = \"mod+q\", action = \"close\" },\n"
	"		{ key = \"mod+f\", action = \"maximize\" },\n"
	"		{ key = \"mod+space\", action = \"floating\" },\n"
	"		{ key = \"mod+j\", action = \"focus_next\" },\n"
	"		{ key = \"mod+k\", action = \"focus_prev\" },\n"
	"		{ key = \"mod+shift+r\", action = \"reload\" }, -- re-read this file\n"
	"		{ key = \"mod+shift+e\", action = \"quit\" },   -- log out\n"
	"	},\n"
	"}\n";

bool
chara_config_write_example(const char *path)
{
	if (!path) {
		errno = EINVAL;
		return false;
	}
	/* The configuration directory may not exist yet on a first run. */
	const char *slash = strrchr(path, '/');
	if (slash && slash != path) {
		char *dir = strndup(path, (size_t)(slash - path));
		if (!dir)
			return false;
		/* Only the last component is created; anything above it, such as
		 * ~/.config, is the caller's business. */
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			free(dir);
			return false;
		}
		free(dir);
	}
	/* O_EXCL: an existing configuration is never overwritten, so this is
	 * safe to call unconditionally. */
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	const char *p = config_example;
	size_t left = sizeof(config_example) - 1;
	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int saved = errno;
			close(fd);
			unlink(path); /* Do not leave a truncated config behind. */
			errno = saved;
			return false;
		}
		p += n;
		left -= (size_t)n;
	}
	if (close(fd) < 0) {
		int saved = errno;
		unlink(path);
		errno = saved;
		return false;
	}
	return true;
}

char *
chara_config_path(const char *filename)
{
	const char *base = getenv("XDG_CONFIG_HOME"), *suffix = "/charawc/";

	if (!base || !*base) {
		base = getenv("HOME");
		suffix = "/.config/charawc/";
	}
	if (!base || !*base) {
		errno = ENOENT;
		return NULL;
	}
	size_t n = strlen(base) + strlen(suffix) + strlen(filename) + 1;
	char *path = malloc(n);
	if (path)
		snprintf(path, n, "%s%s%s", base, suffix, filename);
	return path;
}
