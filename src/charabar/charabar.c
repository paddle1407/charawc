#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <ifaddrs.h>
#include <linux/input-event-codes.h>
#include <linux/memfd.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <net/if.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include "layer-shell.h"
#include "toplevel.h"
#include "workspace.h"

#define MAX_MODULES 16
#define MAX_HITS 128
#define MAX_WORKSPACES 64
#define MAX_WORKSPACE_GROUPS 8
#define MAX_GROUP_OUTPUTS 8
#define TEXT_SIZE 512

/* Which of the compositor's windows a taskbar lists. */
enum taskbar_scope {
	TASKBAR_SCOPE_WORKSPACE, /* only the workspace this monitor is showing */
	TASKBAR_SCOPE_MONITOR,   /* every window of this monitor */
	TASKBAR_SCOPE_ALL,       /* every window of every monitor */
};

/* What a taskbar does once its entries no longer fit beside the other
 * modules. Without this it simply keeps growing and draws over them. */
enum taskbar_overflow {
	TASKBAR_OVERFLOW_SHRINK, /* narrow every entry, then scroll the rest */
	TASKBAR_OVERFLOW_SCROLL, /* keep entry widths and scroll */
	TASKBAR_OVERFLOW_NONE,   /* the old behaviour: overrun the bar */
};

enum module_type {
	MODULE_WORKSPACES,
	MODULE_WINDOW,
	MODULE_TASKBAR,
	MODULE_CLOCK,
	MODULE_CPU,
	MODULE_MEMORY,
	MODULE_NETWORK,
	MODULE_VOLUME,
};

struct bar_config {
	bool top, top_layer, exclusive;
	uint32_t height, padding, spacing;
	uint32_t background, foreground, accent, muted;
	char font[128];
	enum module_type modules[3][MAX_MODULES];
	unsigned module_count[3];
	unsigned workspace_count, window_max, taskbar_max;
	enum taskbar_scope taskbar_scope;
	enum taskbar_overflow taskbar_overflow;
	unsigned taskbar_min_width, taskbar_max_width, taskbar_scroll_step;
	char workspace_format[64], window_empty[128];
	char clock_format[128], cpu_format[64], memory_format[64];
	char network_online[64], network_offline[64];
	char volume_format[64], volume_muted[64];
	unsigned clock_interval, cpu_interval, memory_interval, network_interval;
	unsigned volume_interval;
};

struct output;
struct workspace;
struct toplevel;

enum hit_type { HIT_WORKSPACE, HIT_TOPLEVEL };
struct hit {
	enum hit_type type;
	int x1, x2;
	union {
		struct workspace *workspace;
		struct toplevel *toplevel;
	};
};

struct shm_buffer {
	struct wl_buffer *proxy;
	uint8_t *data;
	struct output *output;
	bool busy;
};

/*
 * Where one monitor's taskbar strip ended up, recomputed before every frame.
 * Both layout passes and the scroll wheel read it, so it cannot live inside
 * render_taskbar().
 */
struct taskbar_layout {
	int budget;    /* pixels the strip may occupy, or -1 when unconstrained */
	int item_cap;  /* pixel ceiling for one entry, or 0 for its natural width */
	int content;   /* total width of the entries at item_cap */
	int offset;    /* how far the strip is scrolled, 0 when it all fits */
	int x, width;  /* the drawn strip, for hit testing the wheel */
};

struct output {
	struct wl_output *proxy;
	uint32_t global_name, bit;
	char name[64];
	struct wl_surface *surface;
	struct wl_callback *frame;
	PangoLayout *layout;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	int shm_fd;
	uint8_t *mapping;
	size_t mapping_size;
	struct shm_buffer buffers[2];
	struct hit hits[MAX_HITS];
	unsigned hit_count;
	struct taskbar_layout taskbar;
	bool configured, dirty, closed;
	struct output *next;
};

/*
 * The compositor publishes one workspace group per monitor, so a panel shows
 * the workspaces of the monitor it is drawn on rather than a single set shared
 * by every screen.
 */
struct wsgroup {
	struct ext_workspace_group_handle_v1 *proxy;
	struct wl_output *outputs[MAX_GROUP_OUTPUTS];
	unsigned output_count;
	bool removed;
};

struct workspace {
	struct ext_workspace_handle_v1 *proxy;
	struct wsgroup *group;
	char name[32];
	uint32_t state, capabilities;
	bool removed;
};

struct toplevel {
	struct zwlr_foreign_toplevel_handle_v1 *proxy;
	char title[TEXT_SIZE], app_id[128];
	uint32_t outputs, last_outputs;
	/* Numbered workspace from the compositor's foreign-toplevel extension,
	 * or zero when it does not publish one. A hidden window enters no output,
	 * so this is the only way to tell which workspace it is waiting on. */
	uint32_t workspace;
	bool active, minimized, maximized, fullscreen, closed;
	struct toplevel *next;
};

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct ext_workspace_manager_v1 *workspace_manager;
	struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
	struct output *outputs, *pointer_output;
	struct workspace workspaces[MAX_WORKSPACES];
	unsigned workspace_count;
	struct wsgroup wsgroups[MAX_WORKSPACE_GROUPS];
	unsigned wsgroup_count;
	struct toplevel *toplevels;
	struct bar_config config;
	char clock_text[TEXT_SIZE], cpu_text[128], memory_text[128], network_text[128];
	char volume_text[128];
	bool volume_muted;
	/* wpctl runs as a child whose pipe joins the main poll set; see
	 * volume_start. -1 when no query is outstanding. */
	int volume_fd;
	pid_t volume_pid;
	time_t volume_started;
	size_t volume_len;
	char volume_buf[256];
	uint64_t cpu_total, cpu_idle;
	int pointer_x;
	bool running;
};

static struct app app;

static void config_defaults(struct bar_config *config)
{
	memset(config, 0, sizeof(*config));
	config->top = true;
	config->top_layer = true;
	config->exclusive = true;
	config->height = 30;
	config->padding = 10;
	config->spacing = 14;
	config->background = 0xff282828;
	config->foreground = 0xffebdbb2;
	config->accent = 0xfffabd2f;
	config->muted = 0xffa89984;
	strcpy(config->font, "MonaspiceRn Nerd Font Regular 10");
	config->modules[0][0] = MODULE_WORKSPACES;
	config->modules[0][1] = MODULE_WINDOW;
	config->module_count[0] = 2;
	config->modules[1][0] = MODULE_CLOCK;
	config->module_count[1] = 1;
	config->modules[2][0] = MODULE_CPU;
	config->modules[2][1] = MODULE_MEMORY;
	config->modules[2][2] = MODULE_NETWORK;
	config->module_count[2] = 3;
	config->workspace_count = 9;
	config->window_max = 64;
	config->taskbar_max = 24;
	config->taskbar_scope = TASKBAR_SCOPE_WORKSPACE;
	config->taskbar_overflow = TASKBAR_OVERFLOW_SHRINK;
	config->taskbar_min_width = 56;
	config->taskbar_max_width = 0; /* zero: as wide as the bar allows */
	config->taskbar_scroll_step = 80;
	strcpy(config->workspace_format, "%n");
	strcpy(config->window_empty, "Desktop");
	strcpy(config->clock_format, "%a %d %b  %H:%M");
	strcpy(config->cpu_format, "CPU %p%");
	strcpy(config->memory_format, "RAM %p%");
	strcpy(config->network_online, "%i");
	strcpy(config->network_offline, "Offline");
	strcpy(config->volume_format, "VOL %p%");
	strcpy(config->volume_muted, "Muted");
	config->clock_interval = 1;
	config->cpu_interval = 2;
	config->memory_interval = 2;
	config->network_interval = 2;
	config->volume_interval = 1;
}

static uint32_t parse_color(const char *text, uint32_t fallback)
{
	char *end;
	unsigned long value;
	if (!text || text[0] != '#' || (strlen(text) != 7 && strlen(text) != 9))
		return fallback;
	errno = 0;
	value = strtoul(text + 1, &end, 16);
	if (errno || *end)
		return fallback;
	return strlen(text) == 7 ? 0xff000000u | (uint32_t)value : (uint32_t)value;
}

static bool lua_field(lua_State *L, int index, const char *name)
{
	index = lua_absindex(L, index);
	lua_getfield(L, index, name);
	if (!lua_isnil(L, -1))
		return true;
	lua_pop(L, 1);
	return false;
}

static int lua_getenv_only(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *value = getenv(name);
	if (value) lua_pushstring(L, value); else lua_pushnil(L);
	return 1;
}

static void lua_string_field(lua_State *L, int index, const char *name,
		char *out, size_t size)
{
	if (!lua_field(L, index, name))
		return;
	if (lua_type(L, -1) == LUA_TSTRING)
		snprintf(out, size, "%s", lua_tostring(L, -1));
	lua_pop(L, 1);
}

static unsigned lua_uint_field(lua_State *L, int index, const char *name,
		unsigned fallback, unsigned min, unsigned max)
{
	unsigned result = fallback;
	if (!lua_field(L, index, name))
		return result;
	lua_Number n = lua_tonumber(L, -1);
	if (lua_type(L, -1) == LUA_TNUMBER && n >= min && n <= max &&
	    n == (unsigned)n)
		result = (unsigned)n;
	lua_pop(L, 1);
	return result;
}

/* Read a string field that names one of a fixed set of values. */
static unsigned lua_enum_field(lua_State *L, int index, const char *name,
		const char *const *names, unsigned count, unsigned fallback)
{
	unsigned result = fallback;
	if (!lua_field(L, index, name))
		return result;
	if (lua_type(L, -1) == LUA_TSTRING) {
		const char *text = lua_tostring(L, -1);
		for (unsigned i = 0; i < count; ++i)
			if (!strcmp(text, names[i])) {
				result = i;
				break;
			}
	}
	lua_pop(L, 1);
	return result;
}

static enum module_type module_from_name(const char *name, bool *valid)
{
	static const struct { const char *name; enum module_type type; } names[] = {
		{ "workspaces", MODULE_WORKSPACES }, { "window", MODULE_WINDOW },
		{ "taskbar", MODULE_TASKBAR }, { "clock", MODULE_CLOCK },
		{ "cpu", MODULE_CPU }, { "memory", MODULE_MEMORY },
		{ "network", MODULE_NETWORK }, { "volume", MODULE_VOLUME },
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
		if (!strcmp(name, names[i].name)) {
			*valid = true;
			return names[i].type;
		}
	*valid = false;
	return MODULE_CLOCK;
}

static void parse_modules(lua_State *L, int index, struct bar_config *config)
{
	static const char *sections[] = { "left", "center", "right" };
	index = lua_absindex(L, index);
	for (unsigned side = 0; side < 3; ++side) {
		if (!lua_field(L, index, sections[side]))
			continue;
		if (lua_istable(L, -1)) {
			unsigned n = (unsigned)lua_rawlen(L, -1);
			if (n > MAX_MODULES)
				n = MAX_MODULES;
			config->module_count[side] = 0;
			for (unsigned i = 1; i <= n; ++i) {
				lua_rawgeti(L, -1, i);
				if (lua_type(L, -1) == LUA_TSTRING) {
					bool valid;
					enum module_type module =
						module_from_name(lua_tostring(L, -1), &valid);
					if (valid)
						config->modules[side][config->module_count[side]++] = module;
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
	}
}

static void parse_timed(lua_State *L, int bar, const char *name, char *format,
		size_t format_size, unsigned *interval)
{
	if (!lua_field(L, bar, name))
		return;
	if (lua_istable(L, -1)) {
		lua_string_field(L, -1, "format", format, format_size);
		*interval = lua_uint_field(L, -1, "interval", *interval, 1, 3600);
	}
	lua_pop(L, 1);
}

/*
 * charaWC parses this same file first and only launches the bar once it passes,
 * but the bar must not be the weaker of the two readers: it reads the file
 * again, by itself, after a reload, and a config that loops or allocates
 * without bound would hang or exhaust the bar rather than the compositor.
 * These limits mirror charaWC's config.c.
 */
struct lua_budget {
	size_t bytes;
	unsigned instructions;
	struct timespec start;
};

static void *config_alloc(void *data, void *ptr, size_t old_size, size_t size)
{
	struct lua_budget *budget = data;
	if (!ptr) old_size = 0; /* Lua passes a type tag here for new objects. */
	if (!size) { free(ptr); budget->bytes -= old_size; return NULL; }
	if (size > 64 * 1024 * 1024 - (budget->bytes - old_size)) return NULL;
	void *next = realloc(ptr, size);
	if (next) budget->bytes = budget->bytes - old_size + size;
	return next;
}

static void config_instruction_hook(lua_State *L, lua_Debug *ar)
{
	void *data;
	struct timespec now;
	(void)ar;
	lua_getallocf(L, &data);
	struct lua_budget *budget = data;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t elapsed = (now.tv_sec - budget->start.tv_sec) * INT64_C(1000000000) +
	                  now.tv_nsec - budget->start.tv_nsec;
	budget->instructions += 1000;
	if (budget->instructions >= 1000000 || elapsed > 250000000)
		luaL_error(L, "configuration exceeded its execution limit");
}

static bool config_load(const char *path, struct bar_config *config)
{
	static struct lua_budget budget;
	budget = (struct lua_budget){0};
	clock_gettime(CLOCK_MONOTONIC, &budget.start);
	lua_State *L = lua_newstate(config_alloc, &budget);
	if (!L)
		return false;
	lua_sethook(L, config_instruction_hook, LUA_MASKCOUNT, 1000);
	const struct { const char *name; lua_CFunction open; } libs[] = {
		{ "_G", luaopen_base }, { "table", luaopen_table },
		{ "string", luaopen_string }, { "math", luaopen_math },
	};
	for (size_t i = 0; i < sizeof(libs) / sizeof(*libs); ++i) {
		luaL_requiref(L, libs[i].name, libs[i].open, 1);
		lua_pop(L, 1);
	}
	/* No arbitrary chunk loading and no package loader, as in charawc. */
	lua_pushnil(L); lua_setglobal(L, "dofile");
	lua_pushnil(L); lua_setglobal(L, "loadfile");
	lua_pushnil(L); lua_setglobal(L, "load");
	lua_newtable(L);
	lua_pushcfunction(L, lua_getenv_only);
	lua_setfield(L, -2, "getenv");
	lua_setglobal(L, "os");
	if (luaL_loadfile(L, path) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
		fprintf(stderr, "charabar: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}
	if (!lua_istable(L, -1) || !lua_field(L, -1, "bar")) {
		lua_close(L);
		return true;
	}
	int bar = lua_gettop(L);
	if (!lua_istable(L, bar)) {
		lua_close(L);
		return false;
	}
	if (lua_field(L, bar, "position")) {
		if (lua_type(L, -1) == LUA_TSTRING)
			config->top = strcmp(lua_tostring(L, -1), "bottom") != 0;
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "layer")) {
		if (lua_type(L, -1) == LUA_TSTRING)
			config->top_layer = strcmp(lua_tostring(L, -1), "bottom") != 0;
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "exclusive")) {
		if (lua_isboolean(L, -1))
			config->exclusive = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	config->height = lua_uint_field(L, bar, "height", config->height, 16, 128);
	config->padding = lua_uint_field(L, bar, "padding", config->padding, 0, 128);
	config->spacing = lua_uint_field(L, bar, "spacing", config->spacing, 0, 128);
	lua_string_field(L, bar, "font", config->font, sizeof(config->font));
	struct { const char *name; uint32_t *value; } colors[] = {
		{ "background", &config->background }, { "foreground", &config->foreground },
		{ "accent", &config->accent }, { "muted", &config->muted },
	};
	for (size_t i = 0; i < sizeof(colors) / sizeof(*colors); ++i) {
		if (lua_field(L, bar, colors[i].name)) {
			if (lua_type(L, -1) == LUA_TSTRING)
				*colors[i].value = parse_color(lua_tostring(L, -1), *colors[i].value);
			lua_pop(L, 1);
		}
	}
	if (lua_field(L, bar, "modules")) {
		if (lua_istable(L, -1))
			parse_modules(L, -1, config);
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "workspaces")) {
		if (lua_istable(L, -1)) {
			config->workspace_count = lua_uint_field(L, -1, "count",
				config->workspace_count, 1, 9);
			lua_string_field(L, -1, "format", config->workspace_format,
			                 sizeof(config->workspace_format));
		}
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "window")) {
		if (lua_istable(L, -1)) {
			config->window_max = lua_uint_field(L, -1, "max_length",
				config->window_max, 1, 512);
			lua_string_field(L, -1, "empty", config->window_empty,
			                 sizeof(config->window_empty));
		}
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "taskbar")) {
		if (lua_istable(L, -1)) {
			static const char *const scopes[] = { "workspace", "monitor", "all" };
			static const char *const overflows[] = { "shrink", "scroll", "none" };
			config->taskbar_max = lua_uint_field(L, -1, "max_length",
				config->taskbar_max, 1, 128);
			config->taskbar_scope = lua_enum_field(L, -1, "scope", scopes,
				sizeof(scopes) / sizeof(*scopes), config->taskbar_scope);
			config->taskbar_overflow = lua_enum_field(L, -1, "overflow",
				overflows, sizeof(overflows) / sizeof(*overflows),
				config->taskbar_overflow);
			config->taskbar_min_width = lua_uint_field(L, -1, "min_width",
				config->taskbar_min_width, 16, 512);
			config->taskbar_max_width = lua_uint_field(L, -1, "max_width",
				config->taskbar_max_width, 0, 16384);
			config->taskbar_scroll_step = lua_uint_field(L, -1, "scroll_step",
				config->taskbar_scroll_step, 1, 1024);
		}
		lua_pop(L, 1);
	}
	parse_timed(L, bar, "clock", config->clock_format,
	            sizeof(config->clock_format), &config->clock_interval);
	parse_timed(L, bar, "cpu", config->cpu_format,
	            sizeof(config->cpu_format), &config->cpu_interval);
	parse_timed(L, bar, "memory", config->memory_format,
	            sizeof(config->memory_format), &config->memory_interval);
	if (lua_field(L, bar, "network")) {
		if (lua_istable(L, -1)) {
			lua_string_field(L, -1, "format_online", config->network_online,
			                 sizeof(config->network_online));
			lua_string_field(L, -1, "format_offline", config->network_offline,
			                 sizeof(config->network_offline));
			config->network_interval = lua_uint_field(L, -1, "interval",
				config->network_interval, 1, 3600);
		}
		lua_pop(L, 1);
	}
	if (lua_field(L, bar, "volume")) {
		if (lua_istable(L, -1)) {
			lua_string_field(L, -1, "format", config->volume_format,
			                 sizeof(config->volume_format));
			lua_string_field(L, -1, "format_muted", config->volume_muted,
			                 sizeof(config->volume_muted));
			/* 0 disables polling: refresh only on SIGUSR1. */
			config->volume_interval = lua_uint_field(L, -1, "interval",
				config->volume_interval, 0, 3600);
		}
		lua_pop(L, 1);
	}
	lua_close(L);
	return true;
}

static void draw_output(struct output *output);

static void draw_all(void)
{
	for (struct output *output = app.outputs; output; output = output->next) {
		output->dirty = true;
		/* Protocol updates may have freed a workspace/window referenced by a
		 * hit. Rebuild hits when the next frame is drawn. */
		output->hit_count = 0;
	}
}

static void draw_pending(void)
{
	for (struct output *output = app.outputs; output; output = output->next)
		draw_output(output);
}

static bool module_enabled(enum module_type type)
{
	for (unsigned side = 0; side < 3; ++side)
		for (unsigned i = 0; i < app.config.module_count[side]; ++i)
			if (app.config.modules[side][i] == type) return true;
	return false;
}

static bool refresh_module(enum module_type type, time_t now, time_t *last,
		unsigned interval, char *text, void (*update)(void))
{
	if (!module_enabled(type) ||
	    (now >= *last && now - *last < (time_t)interval)) return false;
	char previous[TEXT_SIZE];
	snprintf(previous, sizeof(previous), "%s", text);
	update();
	*last = now;
	return strcmp(previous, text) != 0;
}

static void replace_token(char *out, size_t size, const char *format,
		const char *token, const char *replacement)
{
	size_t used = 0, token_length = strlen(token);
	for (const char *p = format; *p && used + 1 < size;) {
		if (!strncmp(p, token, token_length)) {
			size_t length = strlen(replacement);
			if (length > size - used - 1)
				length = size - used - 1;
			memcpy(out + used, replacement, length);
			used += length;
			p += token_length;
		} else {
			out[used++] = *p++;
		}
	}
	out[used] = '\0';
}

static void update_clock(void)
{
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	if (!strftime(app.clock_text, sizeof(app.clock_text), app.config.clock_format,
	              &local))
		app.clock_text[0] = '\0';
}

static void update_cpu(void)
{
	FILE *file = fopen("/proc/stat", "r");
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	if (!file)
		return;
	int count = fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	                   &user, &nice, &system, &idle, &iowait, &irq, &softirq,
	                   &steal);
	fclose(file);
	if (count < 4)
		return;
	uint64_t total = user + nice + system + idle;
	uint64_t idle_total = idle;
	if (count >= 5) { total += iowait; idle_total += iowait; }
	if (count >= 6) total += irq;
	if (count >= 7) total += softirq;
	if (count >= 8) total += steal;
	unsigned percent = 0;
	if (app.cpu_total && total > app.cpu_total) {
		uint64_t delta = total - app.cpu_total;
		uint64_t idle_delta = idle_total - app.cpu_idle;
		percent = (unsigned)((delta - (idle_delta > delta ? delta : idle_delta)) *
		                     100 / delta);
	}
	app.cpu_total = total;
	app.cpu_idle = idle_total;
	char number[16];
	snprintf(number, sizeof(number), "%u", percent);
	replace_token(app.cpu_text, sizeof(app.cpu_text), app.config.cpu_format,
	              "%p", number);
}

static void update_memory(void)
{
	FILE *file = fopen("/proc/meminfo", "r");
	unsigned long long total = 0, available = 0;
	char key[64], unit[16];
	unsigned long long value;
	if (!file)
		return;
	while (fscanf(file, "%63[^:]: %llu %15s\n", key, &value, unit) == 3) {
		if (!strcmp(key, "MemTotal")) total = value;
		else if (!strcmp(key, "MemAvailable")) available = value;
		if (total && available) break;
	}
	fclose(file);
	unsigned percent = total ? (unsigned)((total - available) * 100 / total) : 0;
	char number[16];
	snprintf(number, sizeof(number), "%u", percent);
	replace_token(app.memory_text, sizeof(app.memory_text),
	              app.config.memory_format, "%p", number);
}

static void update_network(void)
{
	struct ifaddrs *addresses = NULL;
	char interface[IF_NAMESIZE] = "";
	if (getifaddrs(&addresses) == 0) {
		for (struct ifaddrs *item = addresses; item; item = item->ifa_next) {
			if (!item->ifa_addr || !(item->ifa_flags & IFF_UP) ||
			    (item->ifa_flags & IFF_LOOPBACK))
				continue;
			int family = item->ifa_addr->sa_family;
			if (family == AF_INET || family == AF_INET6) {
				snprintf(interface, sizeof(interface), "%s", item->ifa_name);
				break;
			}
		}
		freeifaddrs(addresses);
	}
	if (*interface)
		replace_token(app.network_text, sizeof(app.network_text),
		              app.config.network_online, "%i", interface);
	else
		snprintf(app.network_text, sizeof(app.network_text), "%s",
		         app.config.network_offline);
}

/*
 * Ask WirePlumber for the default sink's volume.
 *
 * wpctl prints "Volume: 0.20", plus a trailing " [MUTED]" when the sink is
 * muted. A missing or failed wpctl leaves the previous reading on the bar
 * rather than flashing a wrong number: volume only ever changes because
 * something asked it to, so a stale reading beats a fabricated one.
 *
 * This runs wpctl as a child whose pipe is polled alongside the Wayland fd.
 * Reading it synchronously would stall the bar's event loop for as long as
 * wpctl takes -- and indefinitely if WirePlumber is wedged -- which is
 * exactly the input latency the rest of this file is built to avoid.
 *
 * Because nothing changes the volume behind our back, a periodic query is
 * mostly waste: one wpctl per interval, forever, to re-read a number that
 * only moves when the user asks. SIGUSR1 is the cheaper trigger -- whatever
 * just ran "wpctl set-volume" already knows the reading is stale, and can
 * say so. An interval of 0 turns polling off and leaves the signal as the
 * only trigger; a non-zero interval still polls, for anything that changes
 * the volume without telling us.
 */
#define VOLUME_TIMEOUT 5

static void volume_reap(bool kill_child)
{
	if (app.volume_fd >= 0) {
		close(app.volume_fd);
		app.volume_fd = -1;
	}
	if (app.volume_pid > 0) {
		int flags = kill_child ? 0 : WNOHANG;
		pid_t done;
		if (kill_child)
			kill(app.volume_pid, SIGKILL);
		/*
		 * Normally the child has already exited -- we only get here on EOF
		 * or after killing it. Never block the event loop waiting for one
		 * that somehow has not: kill it and take the guaranteed-prompt wait.
		 */
		while ((done = waitpid(app.volume_pid, NULL, flags)) < 0 &&
		       errno == EINTR)
			;
		if (done == 0) {
			kill(app.volume_pid, SIGKILL);
			while (waitpid(app.volume_pid, NULL, 0) < 0 && errno == EINTR)
				;
		}
		app.volume_pid = -1;
	}
	app.volume_len = 0;
}

static void volume_start(void)
{
	int fds[2];

	if (app.volume_fd >= 0 || app.volume_pid > 0)
		return; /* A previous query is still outstanding. */
	if (pipe(fds) != 0)
		return;
	for (int i = 0; i < 2; ++i) {
		fcntl(fds[i], F_SETFD, FD_CLOEXEC);
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDERR_FILENO);
			if (null > STDERR_FILENO) close(null);
		}
		dup2(fds[1], STDOUT_FILENO);
		close(fds[0]);
		if (fds[1] != STDOUT_FILENO) close(fds[1]);
		execlp("wpctl", "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@",
		       (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	app.volume_fd = fds[0];
	app.volume_pid = pid;
	app.volume_started = time(NULL);
	app.volume_len = 0;
}

/* Parses whatever the child produced. Returns true if the bar must redraw. */
static bool volume_parse(void)
{
	char previous[sizeof(app.volume_text)];
	bool was_muted = app.volume_muted;
	double volume;

	app.volume_buf[app.volume_len] = '\0';
	if (sscanf(app.volume_buf, "Volume: %lf", &volume) != 1)
		return false;

	snprintf(previous, sizeof(previous), "%s", app.volume_text);
	app.volume_muted = strstr(app.volume_buf, "[MUTED]") != NULL;
	if (app.volume_muted && *app.config.volume_muted) {
		snprintf(app.volume_text, sizeof(app.volume_text), "%s",
		         app.config.volume_muted);
	} else {
		/* wpctl reports a fraction, and can exceed 1.0 when something has
		 * raised the sink past 100%. Round to the nearest percent so a 5%
		 * step reads as 5 rather than 4. */
		if (volume < 0) volume = 0;
		char number[16];
		snprintf(number, sizeof(number), "%u", (unsigned)(volume * 100 + 0.5));
		replace_token(app.volume_text, sizeof(app.volume_text),
		              app.config.volume_format, "%p", number);
	}
	/* With an empty format_muted the text reads the same either way and only
	 * the colour changes, so the text diff alone would miss a mute. */
	return strcmp(previous, app.volume_text) != 0 ||
	       was_muted != app.volume_muted;
}

/* Drains the child's pipe. Returns true if a completed reading changed the
 * displayed text. */
static bool volume_poll(bool readable)
{
	bool changed = false;

	if (app.volume_fd < 0)
		return false;
	if (readable) {
		for (;;) {
			ssize_t n = read(app.volume_fd, app.volume_buf + app.volume_len,
			                 sizeof(app.volume_buf) - 1 - app.volume_len);
			if (n > 0) {
				app.volume_len += (size_t)n;
				if (app.volume_len + 1 >= sizeof(app.volume_buf))
					break; /* One line is all we need. */
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return false; /* More to come; keep waiting. */
			break; /* EOF or error: the child is done. */
		}
		changed = volume_parse();
		volume_reap(false);
		return changed;
	}
	/* A wedged WirePlumber must not leave a child and an fd behind forever. */
	if (time(NULL) - app.volume_started >= VOLUME_TIMEOUT)
		volume_reap(true);
	return false;
}

static void set_source(cairo_t *cr, uint32_t color)
{
	double a = ((color >> 24) & 0xff) / 255.0;
	double r = ((color >> 16) & 0xff) / 255.0;
	double g = ((color >> 8) & 0xff) / 255.0;
	double b = (color & 0xff) / 255.0;
	cairo_set_source_rgba(cr, r, g, b, a);
}

static void rounded_rectangle(cairo_t *cr, double x, double y, double width,
		double height, double radius)
{
	if (radius > width / 2) radius = width / 2;
	if (radius > height / 2) radius = height / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + width - radius, y + radius, radius, -1.5708, 0);
	cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, 1.5708);
	cairo_arc(cr, x + radius, y + height - radius, radius, 1.5708, 3.14159);
	cairo_arc(cr, x + radius, y + radius, radius, 3.14159, 4.71239);
	cairo_close_path(cr);
}

static int text_width(PangoLayout *layout, const char *text)
{
	int width;
	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_size(layout, &width, NULL);
	return width;
}

/*
 * Draw text that may not exceed max_width pixels; zero or less leaves it
 * unconstrained. The layout ellipsizes at its end, so a ceiling turns an
 * overlong title into as much of itself as fits followed by an ellipsis.
 * The ceiling is cleared again afterwards: one layout is shared by every
 * module of a monitor.
 */
static int draw_text_within(cairo_t *cr, PangoLayout *layout, const char *text,
		int x, uint32_t color, int max_width, bool draw)
{
	int width, height;
	if (max_width > 0)
		pango_layout_set_width(layout, max_width * PANGO_SCALE);
	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_size(layout, &width, &height);
	if (max_width > 0 && width > max_width)
		width = max_width;
	if (draw) {
		set_source(cr, color);
		cairo_move_to(cr, x, ((int)app.config.height - height) / 2);
		pango_cairo_show_layout(cr, layout);
	}
	if (max_width > 0)
		pango_layout_set_width(layout, -1);
	return width;
}

static int draw_text(cairo_t *cr, PangoLayout *layout, const char *text,
		int x, uint32_t color, bool draw)
{
	return draw_text_within(cr, layout, text, x, color, 0, draw);
}

static void limited_text(char *out, size_t out_size, const char *text,
		unsigned max_length)
{
	glong characters = g_utf8_strlen(text, -1);
	if (characters < 0 || (unsigned long)characters <= max_length) {
		snprintf(out, out_size, "%s", text);
		return;
	}
	if (max_length < 4) {
		const char *end = g_utf8_offset_to_pointer(text, max_length);
		snprintf(out, out_size, "%.*s", (int)(end - text), text);
		return;
	}
	const char *end = g_utf8_offset_to_pointer(text, max_length - 1);
	snprintf(out, out_size, "%.*s…", (int)(end - text), text);
}

static void add_hit(struct output *output, enum hit_type type, int x1, int x2,
		void *object)
{
	if (output->hit_count >= MAX_HITS)
		return;
	struct hit *hit = &output->hits[output->hit_count++];
	hit->type = type;
	hit->x1 = x1;
	hit->x2 = x2;
	if (type == HIT_WORKSPACE)
		hit->workspace = object;
	else
		hit->toplevel = object;
}

/* Whether a group covers the monitor this panel is drawn on. */
static bool group_covers(const struct wsgroup *group, const struct output *output)
{
	if (!group || group->removed)
		return false;
	for (unsigned i = 0; i < group->output_count; ++i)
		if (group->outputs[i] == output->proxy)
			return true;
	return false;
}

static int render_workspaces(struct output *output, cairo_t *cr,
		PangoLayout *layout, int x, bool draw)
{
	int start = x;
	unsigned shown = 0;
	for (unsigned i = 0; i < app.workspace_count &&
	                     shown < app.config.workspace_count; ++i) {
		struct workspace *workspace = &app.workspaces[i];
		if (workspace->removed || !group_covers(workspace->group, output))
			continue;
		char number[16], label[96];
		snprintf(number, sizeof(number), "%u", shown + 1);
		replace_token(label, sizeof(label), app.config.workspace_format, "%n",
		              *workspace->name ? workspace->name : number);
		int width = text_width(layout, label) + 16;
		if (draw) {
			if (workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) {
				set_source(cr, app.config.accent);
				rounded_rectangle(cr, x, 4, width, app.config.height - 8,
				                  (app.config.height - 8) / 2.0);
				cairo_fill(cr);
			}
			draw_text(cr, layout, label, x + 8,
			          workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE ?
			              app.config.background : app.config.foreground,
			          true);
			add_hit(output, HIT_WORKSPACE, x, x + width, workspace);
		}
		x += width + 3;
		++shown;
	}
	return x - start;
}

static struct toplevel *active_toplevel(void)
{
	for (struct toplevel *item = app.toplevels; item; item = item->next)
		if (!item->closed && item->active)
			return item;
	return NULL;
}

static int render_window(cairo_t *cr, PangoLayout *layout, int x, bool draw)
{
	struct toplevel *active = active_toplevel();
	char text[TEXT_SIZE];
	limited_text(text, sizeof(text),
	             active && *active->title ? active->title : app.config.window_empty,
	             app.config.window_max);
	return draw_text(cr, layout, text, x,
	                 active ? app.config.foreground : app.config.muted, draw);
}

#define TASKBAR_ITEM_PADDING 8
#define TASKBAR_ITEM_GAP 4
#define TASKBAR_FADE 14

/*
 * The numbered workspace a monitor is showing, or zero when the compositor
 * publishes none. Workspaces are numbered per monitor, so the number is read
 * from the protocol name the compositor gives them and only falls back to the
 * position within this monitor's group when that name is not a number.
 */
static uint32_t output_active_workspace(const struct output *output)
{
	unsigned shown = 0;
	for (unsigned i = 0; i < app.workspace_count; ++i) {
		const struct workspace *workspace = &app.workspaces[i];
		if (workspace->removed || !group_covers(workspace->group, output))
			continue;
		++shown;
		if (!(workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE))
			continue;
		char *end;
		unsigned long number = strtoul(workspace->name, &end, 10);
		if (*workspace->name && !*end && number && number <= UINT32_MAX)
			return (uint32_t)number;
		return shown;
	}
	return 0;
}

/* Whether one window belongs in this monitor's taskbar. */
static bool taskbar_lists(const struct output *output,
		const struct toplevel *item)
{
	if (item->closed)
		return false;
	if (app.config.taskbar_scope == TASKBAR_SCOPE_ALL)
		return true;
	/* A window hidden by a workspace switch has left every output, so its
	 * last monitor is what still associates it with this panel. */
	uint32_t association = item->outputs ? item->outputs : item->last_outputs;
	if (!(association & output->bit))
		return false;
	if (app.config.taskbar_scope == TASKBAR_SCOPE_MONITOR)
		return true;
	uint32_t active = output_active_workspace(output);
	/* A compositor without the workspace extension reports zero for every
	 * window. List the monitor's windows rather than none of them. */
	if (!active || !item->workspace)
		return true;
	return item->workspace == active;
}

static const char *taskbar_label(const struct toplevel *item, char *out,
		size_t size)
{
	const char *source = *item->title ? item->title : item->app_id;
	limited_text(out, size, *source ? source : "Window", app.config.taskbar_max);
	return out;
}

/* Width of one entry, honouring a pixel ceiling of cap when cap is set. */
static int taskbar_item_width(PangoLayout *layout, const char *text, int cap)
{
	int room = cap > 2 * TASKBAR_ITEM_PADDING ?
		cap - 2 * TASKBAR_ITEM_PADDING : 0;
	int width = draw_text_within(NULL, layout, text, 0, 0, room, false);
	return width + 2 * TASKBAR_ITEM_PADDING;
}

/* Total width of this monitor's entries at a given per-entry ceiling. */
static int taskbar_content_width(const struct output *output,
		PangoLayout *layout, int cap, unsigned *count)
{
	int width = 0;
	unsigned items = 0;
	for (struct toplevel *item = app.toplevels; item; item = item->next) {
		char text[TEXT_SIZE];
		if (!taskbar_lists(output, item))
			continue;
		if (items) width += TASKBAR_ITEM_GAP;
		width += taskbar_item_width(layout, taskbar_label(item, text,
		                                                 sizeof(text)), cap);
		++items;
	}
	if (count) *count = items;
	return width;
}

/*
 * Wash the bar colour over an edge of the strip, opaque at x and clear
 * TASKBAR_FADE pixels inwards, so entries hidden past that edge trail off
 * instead of ending on a cut. 'trailing' fades inwards to the left, for the
 * right-hand end of the strip.
 */
static void taskbar_fade(cairo_t *cr, int x, bool trailing)
{
	uint32_t color = app.config.background;
	double r = ((color >> 16) & 0xff) / 255.0;
	double g = ((color >> 8) & 0xff) / 255.0;
	double b = (color & 0xff) / 255.0;
	double a = ((color >> 24) & 0xff) / 255.0;
	double from = x;
	double to = trailing ? x - TASKBAR_FADE : x + TASKBAR_FADE;
	cairo_pattern_t *fade = cairo_pattern_create_linear(from, 0, to, 0);
	cairo_pattern_add_color_stop_rgba(fade, 0, r, g, b, a);
	cairo_pattern_add_color_stop_rgba(fade, 1, r, g, b, 0);
	cairo_set_source(cr, fade);
	cairo_rectangle(cr, to < from ? to : from, 0, TASKBAR_FADE,
	                app.config.height);
	cairo_fill(cr);
	cairo_pattern_destroy(fade);
}

static int render_taskbar(struct output *output, cairo_t *cr,
		PangoLayout *layout, int x, bool draw)
{
	const struct taskbar_layout *taskbar = &output->taskbar;
	bool clipped = taskbar->budget >= 0 && taskbar->content > taskbar->budget;
	int shown = clipped ? taskbar->budget : taskbar->content;
	if (!draw || shown <= 0)
		return shown > 0 ? shown : 0;
	output->taskbar.x = x;
	output->taskbar.width = shown;
	if (clipped) {
		cairo_save(cr);
		cairo_rectangle(cr, x, 0, shown, app.config.height);
		cairo_clip(cr);
	}
	int item_x = x - (clipped ? taskbar->offset : 0);
	for (struct toplevel *item = app.toplevels; item; item = item->next) {
		char text[TEXT_SIZE];
		if (!taskbar_lists(output, item))
			continue;
		taskbar_label(item, text, sizeof(text));
		int width = taskbar_item_width(layout, text, taskbar->item_cap);
		/* Entries scrolled fully out of the strip still advance the cursor,
		 * but neither paint nor take clicks. */
		if (item_x + width > x && item_x < x + shown) {
			if (item->active) {
				set_source(cr, app.config.accent);
				rounded_rectangle(cr, item_x, 4, width, app.config.height - 8, 6);
				cairo_fill(cr);
			}
			draw_text_within(cr, layout, text, item_x + TASKBAR_ITEM_PADDING,
			                 item->active ? app.config.background :
			                 item->minimized ? app.config.muted :
			                                   app.config.foreground,
			                 taskbar->item_cap ?
			                     width - 2 * TASKBAR_ITEM_PADDING : 0,
			                 true);
			int x1 = item_x < x ? x : item_x;
			int x2 = item_x + width > x + shown ? x + shown : item_x + width;
			if (x2 > x1)
				add_hit(output, HIT_TOPLEVEL, x1, x2, item);
		}
		item_x += width + TASKBAR_ITEM_GAP;
	}
	if (clipped) {
		cairo_restore(cr);
		/* Fade the ends that have entries beyond them, so a strip that can be
		 * scrolled does not look like one that simply stops. */
		if (taskbar->offset > 0)
			taskbar_fade(cr, x, false);
		if (taskbar->offset < taskbar->content - shown)
			taskbar_fade(cr, x + shown, true);
	}
	return shown;
}

static int render_module(enum module_type module, struct output *output,
		cairo_t *cr, PangoLayout *layout, int x, bool draw)
{
	switch (module) {
	case MODULE_WORKSPACES: return render_workspaces(output, cr, layout, x, draw);
	case MODULE_WINDOW: return render_window(cr, layout, x, draw);
	case MODULE_TASKBAR: return render_taskbar(output, cr, layout, x, draw);
	case MODULE_CLOCK: return draw_text(cr, layout, app.clock_text, x, app.config.foreground, draw);
	case MODULE_CPU: return draw_text(cr, layout, app.cpu_text, x, app.config.foreground, draw);
	case MODULE_MEMORY: return draw_text(cr, layout, app.memory_text, x, app.config.foreground, draw);
	case MODULE_NETWORK: return draw_text(cr, layout, app.network_text, x,
	                                      *app.network_text ? app.config.foreground : app.config.muted,
	                                      draw);
	case MODULE_VOLUME: return draw_text(cr, layout, app.volume_text, x,
	                                     app.volume_muted ? app.config.muted : app.config.foreground,
	                                     draw);
	}
	return 0;
}

static int group_width(unsigned side, struct output *output, cairo_t *cr,
		PangoLayout *layout)
{
	int width = 0;
	for (unsigned i = 0; i < app.config.module_count[side]; ++i) {
		if (i) width += app.config.spacing;
		width += render_module(app.config.modules[side][i], output, cr, layout,
		                       0, false);
	}
	return width;
}

/*
 * Width of one section with its taskbars left out, their separators included.
 * This is what the rest of the bar costs, and so what a taskbar has to fit in
 * beside.
 */
static int group_fixed_width(unsigned side, struct output *output, cairo_t *cr,
		PangoLayout *layout)
{
	int width = 0;
	for (unsigned i = 0; i < app.config.module_count[side]; ++i) {
		if (i) width += app.config.spacing;
		if (app.config.modules[side][i] == MODULE_TASKBAR)
			continue;
		width += render_module(app.config.modules[side][i], output, cr, layout,
		                       0, false);
	}
	return width;
}

/*
 * The widest a taskbar in one section may be drawn without moving the other
 * sections. Fitting inside the bar is not enough: the centre section is
 * centred rather than packed against its neighbours, so a growing taskbar on
 * the left would slide the clock rightwards long before the bar ran out of
 * room. A left taskbar therefore stops at where the centre section sits when
 * nothing disturbs it, and only the space before that is its to use.
 */
static int taskbar_allowance(unsigned side, const struct output *output,
		const int *fixed)
{
	int width = (int)output->width;
	int padding = (int)app.config.padding;
	int spacing = (int)app.config.spacing;
	/* Where the centre section rests, for the sections either side of it. */
	bool centred = app.config.module_count[1] && side != 1;
	int centre_left = (width - fixed[1]) / 2;
	int start, limit;
	switch (side) {
	case 0:
		limit = width - padding;
		if (app.config.module_count[2])
			limit -= fixed[2] + spacing;
		if (centred && centre_left - spacing < limit)
			limit = centre_left - spacing;
		return limit - padding - fixed[0];
	case 2:
		start = padding;
		if (app.config.module_count[0])
			start += fixed[0] + spacing;
		if (centred && centre_left + fixed[1] + spacing > start)
			start = centre_left + fixed[1] + spacing;
		return width - padding - start - fixed[2];
	default: {
		/* A centred section grows by half its width in each direction, so
		 * whichever neighbour is closer sets the limit for both. */
		int room = width - 2 * padding;
		if (app.config.module_count[0])
			room = width - 2 * (padding + fixed[0] + spacing);
		if (app.config.module_count[2]) {
			int other = width - 2 * (padding + fixed[2] + spacing);
			if (other < room)
				room = other;
		}
		return room - fixed[1];
	}
	}
}

/*
 * Decide how much room this monitor's taskbar gets and how to fit its entries
 * into it, before either layout pass runs. Every other module is measured
 * first, so a monitor full of windows narrows and then scrolls its entries
 * inside a fixed strip, leaving every other section exactly where it was.
 */
static void layout_taskbar(struct output *output, cairo_t *cr,
		PangoLayout *layout)
{
	struct taskbar_layout *taskbar = &output->taskbar;
	int previous_offset = taskbar->offset;
	unsigned instances[3] = {0}, total = 0, items = 0;
	int fixed[3] = {0};
	*taskbar = (struct taskbar_layout){ .budget = -1 };
	for (unsigned side = 0; side < 3; ++side) {
		if (!app.config.module_count[side])
			continue;
		fixed[side] = group_fixed_width(side, output, cr, layout);
		for (unsigned i = 0; i < app.config.module_count[side]; ++i)
			if (app.config.modules[side][i] == MODULE_TASKBAR)
				++instances[side];
		total += instances[side];
	}
	if (!total)
		return;
	taskbar->content = taskbar_content_width(output, layout, 0, &items);
	if (app.config.taskbar_overflow == TASKBAR_OVERFLOW_NONE)
		return;
	/* Taskbars in more than one section all take the tightest allowance, so
	 * that none of them can be the one that shifts a neighbour. */
	int available = -1;
	for (unsigned side = 0; side < 3; ++side) {
		if (!instances[side])
			continue;
		int allowance = taskbar_allowance(side, output, fixed) /
		                (int)instances[side];
		if (available < 0 || allowance < available)
			available = allowance;
	}
	if (app.config.taskbar_max_width &&
	    (int)app.config.taskbar_max_width < available)
		available = (int)app.config.taskbar_max_width;
	if (available < 0)
		available = 0;
	if (taskbar->content <= available)
		return;
	if (app.config.taskbar_overflow == TASKBAR_OVERFLOW_SHRINK && items) {
		int spare = available - (int)(items - 1) * TASKBAR_ITEM_GAP;
		int cap = spare / (int)items;
		if (cap < (int)app.config.taskbar_min_width)
			cap = (int)app.config.taskbar_min_width;
		taskbar->item_cap = cap;
		taskbar->content = taskbar_content_width(output, layout, cap, NULL);
	}
	taskbar->budget = available;
	int limit = taskbar->content - available;
	taskbar->offset = previous_offset < 0 ? 0 :
	                  previous_offset > limit ? limit : previous_offset;
	if (taskbar->offset < 0)
		taskbar->offset = 0;
}

static void render_group(unsigned side, struct output *output, cairo_t *cr,
		PangoLayout *layout, int x)
{
	for (unsigned i = 0; i < app.config.module_count[side]; ++i) {
		if (i) x += app.config.spacing;
		x += render_module(app.config.modules[side][i], output, cr, layout, x,
		                   true);
	}
}

static bool allocate_buffers(struct output *output, uint32_t width,
		uint32_t height);

static void buffer_release(void *data, struct wl_buffer *proxy)
{
	struct shm_buffer *buffer = data;
	(void)proxy;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static bool allocate_buffers(struct output *output, uint32_t width,
		uint32_t height)
{
	if (!width || !height || width > 16384 || height > 1024)
		return false;
	size_t stride = (size_t)width * 4;
	if (stride / 4 != width || (size_t)height > SIZE_MAX / stride / 2)
		return false;
	size_t frame_size = stride * height;
	size_t size = frame_size * 2;
	int fd = memfd_create("charabar", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0) close(fd);
		return false;
	}
	uint8_t *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, (int)size);
	if (!pool) {
		munmap(mapping, size);
		close(fd);
		return false;
	}
	struct shm_buffer next[2] = {0};
	for (unsigned i = 0; i < 2; ++i) {
		next[i].proxy = wl_shm_pool_create_buffer(
			pool, (int)(i * frame_size), (int)width, (int)height, (int)stride,
			WL_SHM_FORMAT_ARGB8888);
		if (!next[i].proxy) {
			for (unsigned j = 0; j < i; ++j) wl_buffer_destroy(next[j].proxy);
			wl_shm_pool_destroy(pool);
			munmap(mapping, size);
			close(fd);
			return false;
		}
		next[i].data = mapping + i * frame_size;
		next[i].output = output;
	}
	struct shm_buffer old[2] = { output->buffers[0], output->buffers[1] };
	uint8_t *old_mapping = output->mapping;
	size_t old_mapping_size = output->mapping_size;
	int old_fd = output->shm_fd;
	for (unsigned i = 0; i < 2; ++i) {
		output->buffers[i] = next[i];
		wl_buffer_add_listener(output->buffers[i].proxy, &buffer_listener,
		                       &output->buffers[i]);
	}
	wl_shm_pool_destroy(pool);
	for (unsigned i = 0; i < 2; ++i)
		if (old[i].proxy)
			wl_buffer_destroy(old[i].proxy);
	/* The server owns its mapping of this backing file. Unmapping our old
	 * address doesn't change its contents or invalidate the server's view. */
	if (old_mapping)
		munmap(old_mapping, old_mapping_size);
	if (old_fd >= 0)
		close(old_fd);
	output->mapping = mapping;
	output->mapping_size = size;
	output->shm_fd = fd;
	output->width = width;
	output->height = height;
	return true;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct output *output = data;
	(void)time;
	wl_callback_destroy(callback);
	output->frame = NULL;
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void draw_output(struct output *output)
{
	if (!output || !output->configured || output->closed || !output->width ||
	    !output->dirty || output->frame)
		return;
	struct shm_buffer *buffer = NULL;
	for (unsigned i = 0; i < 2; ++i)
		if (output->buffers[i].proxy && !output->buffers[i].busy) {
			buffer = &output->buffers[i];
			break;
		}
	if (!buffer)
		return;
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		buffer->data, CAIRO_FORMAT_ARGB32, (int)output->width,
		(int)output->height, (int)output->width * 4);
	cairo_t *cr = cairo_create(surface);
	set_source(cr, app.config.background);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	if (!output->layout) {
		output->layout = pango_cairo_create_layout(cr);
		PangoFontDescription *font =
			pango_font_description_from_string(app.config.font);
		pango_layout_set_font_description(output->layout, font);
		pango_font_description_free(font);
		pango_layout_set_ellipsize(output->layout, PANGO_ELLIPSIZE_END);
	} else {
		pango_cairo_update_layout(cr, output->layout);
	}
	PangoLayout *layout = output->layout;
	output->hit_count = 0;
	layout_taskbar(output, cr, layout);
	int left_width = group_width(0, output, cr, layout);
	int center_width = group_width(1, output, cr, layout);
	int right_width = group_width(2, output, cr, layout);
	int left = (int)app.config.padding;
	int center = ((int)output->width - center_width) / 2;
	int right = (int)output->width - (int)app.config.padding - right_width;
	if (center < left + left_width + (int)app.config.spacing)
		center = left + left_width + app.config.spacing;
	if (center + center_width + (int)app.config.spacing > right)
		center = right - center_width - app.config.spacing;
	render_group(0, output, cr, layout, left);
	if (center >= 0)
		render_group(1, output, cr, layout, center);
	if (right >= 0)
		render_group(2, output, cr, layout, right);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);
	buffer->busy = true;
	output->dirty = false;
	wl_surface_attach(output->surface, buffer->proxy, 0, 0);
	wl_surface_damage(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	output->frame = wl_surface_frame(output->surface);
	wl_callback_add_listener(output->frame, &frame_listener, output);
	struct wl_region *opaque = wl_compositor_create_region(app.compositor);
	if ((app.config.background >> 24) == 0xff)
		wl_region_add(opaque, 0, 0, (int)output->width, (int)output->height);
	wl_surface_set_opaque_region(output->surface, opaque);
	wl_region_destroy(opaque);
	wl_surface_commit(output->surface);
}

static void layer_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
		uint32_t width, uint32_t height)
{
	struct output *output = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	if (!width) width = output->width;
	if (!height) height = app.config.height;
	if ((!output->mapping || output->width != width || output->height != height) &&
	    !allocate_buffers(output, width, height)) {
		fprintf(stderr, "charabar: couldn't allocate %ux%u bar buffers\n", width,
		        height);
		return;
	}
	output->configured = true;
	output->dirty = true;
}

static void layer_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface)
{
	struct output *output = data;
	(void)layer_surface;
	output->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static bool create_bar(struct output *output)
{
	output->surface = wl_compositor_create_surface(app.compositor);
	if (!output->surface)
		return false;
	wl_surface_set_user_data(output->surface, output);
	uint32_t layer = app.config.top_layer ?
		ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		app.layer_shell, output->surface, output->proxy, layer, "charabar");
	if (!output->layer_surface)
		return false;
	zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_listener,
	                                   output);
	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	                  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
	                  (app.config.top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP :
	                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, app.config.height);
	zwlr_layer_surface_v1_set_exclusive_zone(
		output->layer_surface, app.config.exclusive ? (int)app.config.height : 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		output->layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(output->surface);
	return true;
}

static void output_geometry(void *data, struct wl_output *proxy, int32_t x,
		int32_t y, int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	(void)data; (void)proxy; (void)x; (void)y; (void)physical_width;
	(void)physical_height; (void)subpixel; (void)make; (void)model;
	(void)transform;
}

static void output_mode(void *data, struct wl_output *proxy, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh)
{
	(void)data; (void)proxy; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *proxy)
{ (void)data; (void)proxy; }
static void output_scale(void *data, struct wl_output *proxy, int32_t factor)
{ (void)data; (void)proxy; (void)factor; }
static void output_name(void *data, struct wl_output *proxy, const char *name)
{ (void)proxy; snprintf(((struct output *)data)->name,
	                    sizeof(((struct output *)data)->name), "%s", name); }
static void output_description(void *data, struct wl_output *proxy,
		const char *description)
{ (void)data; (void)proxy; (void)description; }

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static struct output *find_output(struct wl_output *proxy)
{
	for (struct output *output = app.outputs; output; output = output->next)
		if (output->proxy == proxy)
			return output;
	return NULL;
}

static struct workspace *find_workspace(struct ext_workspace_handle_v1 *proxy)
{
	for (unsigned i = 0; i < app.workspace_count; ++i)
		if (app.workspaces[i].proxy == proxy)
			return &app.workspaces[i];
	return NULL;
}

static void workspace_id(void *data, struct ext_workspace_handle_v1 *proxy,
		const char *id)
{ (void)data; (void)proxy; (void)id; }

static void workspace_name(void *data, struct ext_workspace_handle_v1 *proxy,
		const char *name)
{
	struct workspace *workspace = data;
	(void)proxy;
	snprintf(workspace->name, sizeof(workspace->name), "%s", name);
}

static void workspace_coordinates(void *data,
		struct ext_workspace_handle_v1 *proxy, struct wl_array *coordinates)
{ (void)data; (void)proxy; (void)coordinates; }

static void workspace_state(void *data, struct ext_workspace_handle_v1 *proxy,
		uint32_t state)
{
	struct workspace *workspace = data;
	(void)proxy;
	workspace->state = state;
}

static void workspace_capabilities(void *data,
		struct ext_workspace_handle_v1 *proxy, uint32_t capabilities)
{
	struct workspace *workspace = data;
	(void)proxy;
	workspace->capabilities = capabilities;
}

static void workspace_removed(void *data,
		struct ext_workspace_handle_v1 *proxy)
{
	struct workspace *workspace = data;
	workspace->removed = true;
	workspace->group = NULL;
	ext_workspace_handle_v1_destroy(proxy);
	workspace->proxy = NULL;
	for (struct output *output = app.outputs; output; output = output->next)
		output->hit_count = 0;
	draw_all();
}

static const struct ext_workspace_handle_v1_listener workspace_listener = {
	.id = workspace_id,
	.name = workspace_name,
	.coordinates = workspace_coordinates,
	.state = workspace_state,
	.capabilities = workspace_capabilities,
	.removed = workspace_removed,
};

static void group_capabilities(void *data,
		struct ext_workspace_group_handle_v1 *group, uint32_t capabilities)
{ (void)data; (void)group; (void)capabilities; }

static void group_output_enter(void *data,
		struct ext_workspace_group_handle_v1 *group, struct wl_output *output)
{
	struct wsgroup *wsgroup = data;
	(void)group;
	for (unsigned i = 0; i < wsgroup->output_count; ++i)
		if (wsgroup->outputs[i] == output)
			return;
	if (wsgroup->output_count < MAX_GROUP_OUTPUTS)
		wsgroup->outputs[wsgroup->output_count++] = output;
}

static void group_output_leave(void *data,
		struct ext_workspace_group_handle_v1 *group, struct wl_output *output)
{
	struct wsgroup *wsgroup = data;
	(void)group;
	for (unsigned i = 0; i < wsgroup->output_count; ++i) {
		if (wsgroup->outputs[i] != output)
			continue;
		wsgroup->outputs[i] = wsgroup->outputs[--wsgroup->output_count];
		return;
	}
}

static void group_workspace_enter(void *data,
		struct ext_workspace_group_handle_v1 *group,
		struct ext_workspace_handle_v1 *proxy)
{
	(void)group;
	struct workspace *workspace = find_workspace(proxy);
	if (workspace) workspace->group = data;
}

static void group_workspace_leave(void *data,
		struct ext_workspace_group_handle_v1 *group,
		struct ext_workspace_handle_v1 *proxy)
{
	(void)group;
	struct workspace *workspace = find_workspace(proxy);
	if (workspace && workspace->group == data) workspace->group = NULL;
}

static void group_removed(void *data,
		struct ext_workspace_group_handle_v1 *group)
{
	struct wsgroup *wsgroup = data;
	ext_workspace_group_handle_v1_destroy(group);
	wsgroup->proxy = NULL;
	wsgroup->removed = true;
	wsgroup->output_count = 0;
	for (unsigned i = 0; i < app.workspace_count; ++i)
		if (app.workspaces[i].group == wsgroup) app.workspaces[i].group = NULL;
	draw_all();
}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = group_capabilities,
	.output_enter = group_output_enter,
	.output_leave = group_output_leave,
	.workspace_enter = group_workspace_enter,
	.workspace_leave = group_workspace_leave,
	.removed = group_removed,
};

static void manager_workspace_group(void *data,
		struct ext_workspace_manager_v1 *manager,
		struct ext_workspace_group_handle_v1 *group)
{
	(void)data; (void)manager;
	unsigned slot;
	for (slot = 0; slot < app.wsgroup_count; ++slot)
		if (!app.wsgroups[slot].proxy) break;
	if (slot >= MAX_WORKSPACE_GROUPS) {
		ext_workspace_group_handle_v1_destroy(group);
		return;
	}
	if (slot == app.wsgroup_count) ++app.wsgroup_count;
	struct wsgroup *wsgroup = &app.wsgroups[slot];
	memset(wsgroup, 0, sizeof(*wsgroup));
	wsgroup->proxy = group;
	ext_workspace_group_handle_v1_add_listener(group, &group_listener, wsgroup);
}

static void manager_workspace(void *data,
		struct ext_workspace_manager_v1 *manager,
		struct ext_workspace_handle_v1 *proxy)
{
	(void)data; (void)manager;
	unsigned slot;
	for (slot = 0; slot < app.workspace_count; ++slot)
		if (!app.workspaces[slot].proxy) break;
	if (slot >= MAX_WORKSPACES) {
		ext_workspace_handle_v1_destroy(proxy);
		return;
	}
	if (slot == app.workspace_count) ++app.workspace_count;
	struct workspace *workspace = &app.workspaces[slot];
	memset(workspace, 0, sizeof(*workspace));
	workspace->proxy = proxy;
	ext_workspace_handle_v1_add_listener(proxy, &workspace_listener, workspace);
}

static void workspace_manager_done(void *data,
		struct ext_workspace_manager_v1 *manager)
{ (void)data; (void)manager; draw_all(); }

static void workspace_manager_finished(void *data,
		struct ext_workspace_manager_v1 *manager)
{
	(void)data;
	/*
	 * The workspace and group objects are gone with the manager. Dropping
	 * only the manager would leave click targets pointing at handles whose
	 * proxies can no longer carry a request, and the commit below would be
	 * made on a NULL manager.
	 */
	for (unsigned i = 0; i < app.workspace_count; ++i) {
		if (app.workspaces[i].proxy) {
			ext_workspace_handle_v1_destroy(app.workspaces[i].proxy);
			app.workspaces[i].proxy = NULL;
		}
	}
	for (unsigned i = 0; i < app.wsgroup_count; ++i) {
		if (app.wsgroups[i].proxy) {
			ext_workspace_group_handle_v1_destroy(app.wsgroups[i].proxy);
			app.wsgroups[i].proxy = NULL;
		}
	}
	ext_workspace_manager_v1_destroy(manager);
	app.workspace_manager = NULL;
	draw_all();
}

static const struct ext_workspace_manager_v1_listener workspace_manager_listener = {
	.workspace_group = manager_workspace_group,
	.workspace = manager_workspace,
	.done = workspace_manager_done,
	.finished = workspace_manager_finished,
};

static void toplevel_title(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, const char *title)
{
	struct toplevel *toplevel = data;
	(void)proxy;
	snprintf(toplevel->title, sizeof(toplevel->title), "%s", title);
}

static void toplevel_app_id(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, const char *app_id)
{
	struct toplevel *toplevel = data;
	(void)proxy;
	snprintf(toplevel->app_id, sizeof(toplevel->app_id), "%s", app_id);
}

/*
 * last_outputs is where the window was the last time it was on any monitor at
 * all, and is what associates it with a panel once it is hidden. It has to be
 * replaced rather than accumulated: a window that has at some point been on
 * both monitors would otherwise keep both bits for the rest of its life, and
 * every workspace switch would then list it on both panels at once.
 */
static void toplevel_output_enter(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, struct wl_output *wl_output)
{
	struct toplevel *toplevel = data;
	struct output *output = find_output(wl_output);
	(void)proxy;
	if (output) {
		toplevel->outputs |= output->bit;
		toplevel->last_outputs = toplevel->outputs;
	}
}

static void toplevel_output_leave(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, struct wl_output *wl_output)
{
	struct toplevel *toplevel = data;
	struct output *output = find_output(wl_output);
	(void)proxy;
	if (!output) return;
	toplevel->outputs &= ~output->bit;
	/* Leaving the last one is what makes that monitor the remembered one. */
	toplevel->last_outputs = toplevel->outputs ? toplevel->outputs : output->bit;
}

static void toplevel_state(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, struct wl_array *states)
{
	struct toplevel *toplevel = data;
	uint32_t *state;
	(void)proxy;
	toplevel->active = toplevel->minimized = false;
	toplevel->maximized = toplevel->fullscreen = false;
	wl_array_for_each(state, states) {
		switch (*state) {
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: toplevel->active = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: toplevel->minimized = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: toplevel->maximized = true; break;
		case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: toplevel->fullscreen = true; break;
		}
	}
}

static void toplevel_done(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy)
{ (void)data; (void)proxy; draw_all(); }

static void toplevel_closed(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy)
{
	struct toplevel *toplevel = data, **link = &app.toplevels;
	while (*link && *link != toplevel) link = &(*link)->next;
	if (*link) *link = toplevel->next;
	/* Redraw can be deferred by a frame callback or busy SHM buffers. Remove
	 * old click targets before freeing the window, even if pixels remain. */
	for (struct output *output = app.outputs; output; output = output->next) {
		unsigned count = 0;
		for (unsigned i = 0; i < output->hit_count; ++i) {
			struct hit *hit = &output->hits[i];
			if (hit->type != HIT_TOPLEVEL || hit->toplevel != toplevel)
				output->hits[count++] = *hit;
		}
		output->hit_count = count;
	}
	toplevel->closed = true;
	zwlr_foreign_toplevel_handle_v1_destroy(proxy);
	free(toplevel);
	draw_all();
}

static void toplevel_workspace(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy, uint32_t workspace)
{
	struct toplevel *toplevel = data;
	(void)proxy;
	toplevel->workspace = workspace;
}

static void toplevel_parent(void *data,
		struct zwlr_foreign_toplevel_handle_v1 *proxy,
		struct zwlr_foreign_toplevel_handle_v1 *parent)
{ (void)data; (void)proxy; (void)parent; }

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.title = toplevel_title,
	.app_id = toplevel_app_id,
	.output_enter = toplevel_output_enter,
	.output_leave = toplevel_output_leave,
	.state = toplevel_state,
	.done = toplevel_done,
	.closed = toplevel_closed,
	.parent = toplevel_parent,
	.workspace = toplevel_workspace,
};

static void manager_toplevel(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *proxy)
{
	(void)data; (void)manager;
	struct toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		zwlr_foreign_toplevel_handle_v1_destroy(proxy);
		return;
	}
	toplevel->proxy = proxy;
	toplevel->next = app.toplevels;
	app.toplevels = toplevel;
	zwlr_foreign_toplevel_handle_v1_add_listener(proxy, &toplevel_listener,
	                                             toplevel);
}

static void toplevel_manager_finished(void *data,
		struct zwlr_foreign_toplevel_manager_v1 *manager)
{
	(void)data;
	zwlr_foreign_toplevel_manager_v1_destroy(manager);
	app.toplevel_manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
	.toplevel = manager_toplevel,
	.finished = toplevel_manager_finished,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data; (void)pointer; (void)serial; (void)sy;
	app.pointer_output = wl_surface_get_user_data(surface);
	app.pointer_x = wl_fixed_to_int(sx);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface)
{
	(void)data; (void)pointer; (void)serial; (void)surface;
	app.pointer_output = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data; (void)pointer; (void)time; (void)sy;
	app.pointer_x = wl_fixed_to_int(sx);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)pointer; (void)serial; (void)time;
	if (state != WL_POINTER_BUTTON_STATE_RELEASED || !app.pointer_output)
		return;
	for (unsigned i = 0; i < app.pointer_output->hit_count; ++i) {
		struct hit *hit = &app.pointer_output->hits[i];
		if (app.pointer_x < hit->x1 || app.pointer_x >= hit->x2)
			continue;
		if (hit->type == HIT_WORKSPACE && button == BTN_LEFT &&
		    app.workspace_manager && hit->workspace->proxy &&
		    (hit->workspace->capabilities &
		     EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE)) {
			ext_workspace_handle_v1_activate(hit->workspace->proxy);
			ext_workspace_manager_v1_commit(app.workspace_manager);
		} else if (hit->type == HIT_TOPLEVEL && hit->toplevel->proxy) {
			if (button == BTN_LEFT) {
				if (hit->toplevel->minimized)
					zwlr_foreign_toplevel_handle_v1_unset_minimized(
						hit->toplevel->proxy);
				zwlr_foreign_toplevel_handle_v1_activate(hit->toplevel->proxy,
				                                                app.seat);
			} else if (button == BTN_MIDDLE) {
				if (hit->toplevel->minimized)
					zwlr_foreign_toplevel_handle_v1_unset_minimized(
						hit->toplevel->proxy);
				else
					zwlr_foreign_toplevel_handle_v1_set_minimized(
						hit->toplevel->proxy);
			} else if (button == BTN_RIGHT) {
				zwlr_foreign_toplevel_handle_v1_close(hit->toplevel->proxy);
			}
		}
		wl_display_flush(app.display);
		break;
	}
}

/*
 * Scrolling is accumulated across one pointer frame rather than acted on per
 * event: a wheel sends both a discrete notch and the distance it stands for,
 * and only the notch is a sensible scroll step. A touchpad sends the distance
 * alone, which already is one.
 */
static struct {
	double distance;
	int32_t notches;
} axis_frame;

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)pointer; (void)time; (void)axis;
	/* Either wheel scrolls the strip; there is only one direction to go. */
	axis_frame.distance += wl_fixed_to_double(value);
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
		uint32_t axis, int32_t discrete)
{
	(void)data; (void)pointer; (void)axis;
	axis_frame.notches += discrete;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
	struct output *output = app.pointer_output;
	int32_t notches = axis_frame.notches;
	double distance = axis_frame.distance;
	(void)data; (void)pointer;
	axis_frame.notches = 0;
	axis_frame.distance = 0;
	if (!output || (!notches && distance == 0))
		return;
	struct taskbar_layout *taskbar = &output->taskbar;
	int limit = taskbar->content - taskbar->budget;
	if (taskbar->budget < 0 || limit <= 0 || !taskbar->width ||
	    app.pointer_x < taskbar->x ||
	    app.pointer_x >= taskbar->x + taskbar->width)
		return;
	int delta = notches ? notches * (int)app.config.taskbar_scroll_step :
	                      (int)distance;
	int offset = taskbar->offset + delta;
	if (offset < 0) offset = 0;
	if (offset > limit) offset = limit;
	if (offset == taskbar->offset)
		return;
	taskbar->offset = offset;
	/* The entries under the pointer move, so the old click targets go with
	 * them. The main loop repaints. */
	output->hit_count = 0;
	output->dirty = true;
}

/* The seat is bound at version 5: every pointer event introduced through
 * version 5 needs a listener. */
static void pointer_axis_source(void *data, struct wl_pointer *pointer,
		uint32_t source)
{ (void)data; (void)pointer; (void)source; }
static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis)
{ (void)data; (void)pointer; (void)time; (void)axis; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities)
{
	(void)data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
		app.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app.pointer) {
		wl_pointer_destroy(app.pointer);
		app.pointer = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static uint32_t available_output_bit(void)
{
	uint32_t used = 0;
	for (struct output *output = app.outputs; output; output = output->next)
		used |= output->bit;
	for (unsigned i = 0; i < 32; ++i)
		if (!(used & (1u << i))) return 1u << i;
	return 0;
}

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	(void)data;
#define BIND(type, iface, maximum)                                             \
	((struct type *)wl_registry_bind(registry, name, &iface,                    \
	                                 version < (maximum) ? version : (maximum)))
	if (!strcmp(interface, wl_compositor_interface.name)) {
		app.compositor = BIND(wl_compositor, wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		app.shm = BIND(wl_shm, wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !app.seat) {
		app.seat = BIND(wl_seat, wl_seat_interface, 5);
		if (app.seat)
			wl_seat_add_listener(app.seat, &seat_listener, NULL);
	} else if (!strcmp(interface, wl_output_interface.name) && available_output_bit()) {
		struct output *output = calloc(1, sizeof(*output));
		if (!output) return;
		output->proxy = BIND(wl_output, wl_output_interface, 4);
		if (!output->proxy) {
			free(output);
			return;
		}
		output->global_name = name;
		output->bit = available_output_bit();
		output->shm_fd = -1;
		snprintf(output->name, sizeof(output->name), "output-%u", name);
		wl_output_add_listener(output->proxy, &output_listener, output);
		output->next = app.outputs;
		app.outputs = output;
		if (app.running && app.compositor && app.layer_shell && app.shm &&
		    !create_bar(output)) {
			fprintf(stderr, "charabar: couldn't create bar for new output\n");
			output->closed = true;
		}
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		app.layer_shell = BIND(zwlr_layer_shell_v1,
		                       zwlr_layer_shell_v1_interface, 4);
	} else if (!strcmp(interface, ext_workspace_manager_v1_interface.name)) {
		app.workspace_manager = BIND(ext_workspace_manager_v1,
		                              ext_workspace_manager_v1_interface, 1);
		if (app.workspace_manager)
			ext_workspace_manager_v1_add_listener(
				app.workspace_manager, &workspace_manager_listener, NULL);
	} else if (!strcmp(interface,
	                  zwlr_foreign_toplevel_manager_v1_interface.name)) {
		app.toplevel_manager = BIND(
			zwlr_foreign_toplevel_manager_v1,
			zwlr_foreign_toplevel_manager_v1_interface, 4);
		if (app.toplevel_manager)
			zwlr_foreign_toplevel_manager_v1_add_listener(
				app.toplevel_manager, &toplevel_manager_listener, NULL);
	}
#undef BIND
}

static void destroy_output_bar(struct output *output)
{
	output->closed = true;
	if (output->frame) {
		wl_callback_destroy(output->frame);
		output->frame = NULL;
	}
	if (output->layout) {
		g_object_unref(output->layout);
		output->layout = NULL;
	}
	if (output->layer_surface) {
		zwlr_layer_surface_v1_destroy(output->layer_surface);
		output->layer_surface = NULL;
	}
	if (output->surface) {
		wl_surface_destroy(output->surface);
		output->surface = NULL;
	}
}

static void destroy_output(struct output *output)
{
	destroy_output_bar(output);
	for (unsigned i = 0; i < 2; ++i)
		if (output->buffers[i].proxy)
			wl_buffer_destroy(output->buffers[i].proxy);
	if (output->mapping)
		munmap(output->mapping, output->mapping_size);
	if (output->shm_fd >= 0) close(output->shm_fd);
	if (output->proxy) {
		if (wl_output_get_version(output->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
			wl_output_release(output->proxy);
		else
			wl_output_destroy(output->proxy);
	}
	free(output);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name)
{
	(void)data; (void)registry;
	for (struct output **link = &app.outputs; *link; link = &(*link)->next) {
		struct output *output = *link;
		if (output->global_name != name)
			continue;
		if (app.pointer_output == output)
			app.pointer_output = NULL;
		for (unsigned i = 0; i < app.wsgroup_count; ++i)
			group_output_leave(&app.wsgroups[i], app.wsgroups[i].proxy,
			                   output->proxy);
		for (struct toplevel *item = app.toplevels; item; item = item->next) {
			item->outputs &= ~output->bit;
			item->last_outputs &= ~output->bit;
		}
		*link = output->next;
		destroy_output(output);
		draw_all();
		break;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t volume_refresh_requested;
static void handle_signal(int signal_number)
{
	if (signal_number == SIGUSR1)
		volume_refresh_requested = 1;
	else
		stop_requested = 1;
}

static void cleanup_app(void)
{
	struct toplevel *toplevel = app.toplevels;
	/* Never leave a wpctl child or its pipe behind. */
	volume_reap(true);
	while (toplevel) {
		struct toplevel *next = toplevel->next;
		if (toplevel->proxy)
			zwlr_foreign_toplevel_handle_v1_destroy(toplevel->proxy);
		free(toplevel);
		toplevel = next;
	}
	for (unsigned i = 0; i < app.workspace_count; ++i)
		if (app.workspaces[i].proxy)
			ext_workspace_handle_v1_destroy(app.workspaces[i].proxy);
	for (unsigned i = 0; i < app.wsgroup_count; ++i)
		if (app.wsgroups[i].proxy)
			ext_workspace_group_handle_v1_destroy(app.wsgroups[i].proxy);
	struct output *output = app.outputs;
	while (output) {
		struct output *next = output->next;
		destroy_output(output);
		output = next;
	}
	if (app.pointer) wl_pointer_destroy(app.pointer);
	if (app.seat) wl_seat_destroy(app.seat);
	if (app.toplevel_manager)
		zwlr_foreign_toplevel_manager_v1_destroy(app.toplevel_manager);
	if (app.workspace_manager)
		ext_workspace_manager_v1_destroy(app.workspace_manager);
	if (app.layer_shell) zwlr_layer_shell_v1_destroy(app.layer_shell);
	if (app.shm) wl_shm_destroy(app.shm);
	if (app.compositor) wl_compositor_destroy(app.compositor);
	if (app.registry) wl_registry_destroy(app.registry);
	if (app.display) wl_display_disconnect(app.display);
}

static char *default_config_path(void)
{
	const char *base = getenv("XDG_CONFIG_HOME");
	char *path;
	if (base && *base) {
		if (asprintf(&path, "%s/charawc/config.lua", base) < 0) return NULL;
	} else {
		const char *home = getenv("HOME");
		if (!home || asprintf(&path, "%s/.config/charawc/config.lua", home) < 0)
			return NULL;
	}
	return path;
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	char *owned_path = NULL;
	if (argc == 3 && !strcmp(argv[1], "-c"))
		config_path = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "Usage: charabar [-c config.lua]\n");
		return 2;
	}
	app.volume_fd = -1;
	app.volume_pid = -1;
	config_defaults(&app.config);
	if (!config_path) {
		owned_path = default_config_path();
		config_path = owned_path;
	}
	if (config_path && access(config_path, R_OK) == 0 &&
	    !config_load(config_path, &app.config)) {
		free(owned_path);
		return 1;
	}
	free(owned_path);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGUSR1, handle_signal);
	app.display = wl_display_connect(NULL);
	if (!app.display) {
		fprintf(stderr, "charabar: couldn't connect to Wayland display\n");
		return 1;
	}
	app.registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(app.registry, &registry_listener, NULL);
	if (wl_display_roundtrip(app.display) < 0 ||
	    wl_display_roundtrip(app.display) < 0)
		goto failed;
	if (!app.compositor || !app.shm || !app.layer_shell || !app.seat ||
	    !app.workspace_manager || !app.toplevel_manager || !app.outputs) {
		fprintf(stderr, "charabar: compositor is missing a required desktop protocol\n");
		goto failed;
	}
	if (module_enabled(MODULE_CLOCK)) update_clock();
	if (module_enabled(MODULE_CPU)) update_cpu();
	if (module_enabled(MODULE_MEMORY)) update_memory();
	if (module_enabled(MODULE_NETWORK)) update_network();
	if (module_enabled(MODULE_VOLUME)) volume_start();
	for (struct output *output = app.outputs; output; output = output->next)
		if (!output->closed && !create_bar(output)) {
			fprintf(stderr, "charabar: couldn't create bar for %s\n", output->name);
			goto failed;
		}
	wl_display_flush(app.display);
	app.running = true;
	time_t last_clock = time(NULL), last_cpu = last_clock,
	       last_memory = last_clock, last_network = last_clock,
	       last_volume = last_clock;
	while (app.running && !stop_requested) {
		if (wl_display_dispatch_pending(app.display) < 0)
			break;
		/* Process a batch of protocol updates before rendering, at most once
		 * per output frame. Never render from individual event listeners. */
		draw_pending();
		struct pollfd pollfds[2] = {
			{ wl_display_get_fd(app.display), POLLIN, 0 },
			{ app.volume_fd, POLLIN, 0 },
		};
		nfds_t nfds = app.volume_fd >= 0 ? 2 : 1;
		if (wl_display_flush(app.display) < 0) {
			if (errno != EAGAIN) break;
			pollfds[0].events |= POLLOUT;
		}
		int result = poll(pollfds, nfds, 250);
		if (result < 0 && errno != EINTR)
			break;
		if (result > 0 && (pollfds[0].revents & (POLLERR | POLLHUP)))
			break;
		if (result > 0 && (pollfds[0].revents & POLLIN) &&
		    wl_display_dispatch(app.display) < 0)
			break;
		bool volume_changed = volume_poll(
			nfds == 2 && result > 0 &&
			(pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)));
		time_t now = time(NULL);
		bool changed = false;
		changed |= refresh_module(MODULE_CLOCK, now, &last_clock,
			app.config.clock_interval, app.clock_text, update_clock);
		changed |= refresh_module(MODULE_CPU, now, &last_cpu,
			app.config.cpu_interval, app.cpu_text, update_cpu);
		changed |= refresh_module(MODULE_MEMORY, now, &last_memory,
			app.config.memory_interval, app.memory_text, update_memory);
		changed |= refresh_module(MODULE_NETWORK, now, &last_network,
			app.config.network_interval, app.network_text, update_network);
		/* The reading arrives later, through volume_poll; this only starts
		 * the query, when SIGUSR1 asks for one or the interval is up. */
		if (module_enabled(MODULE_VOLUME)) {
			bool busy = app.volume_fd >= 0 || app.volume_pid > 0;
			bool due = app.config.volume_interval > 0 &&
			           (now < last_volume ||
			            now - last_volume >=
			                (time_t)app.config.volume_interval);
			/*
			 * A request that arrives mid-query stays pending rather than
			 * being dropped: the outstanding wpctl was started before the
			 * change and would strand the bar on the old reading.
			 */
			if (!busy && (volume_refresh_requested || due)) {
				volume_refresh_requested = 0;
				volume_start();
				last_volume = now;
			}
		}
		changed |= volume_changed;
		if (changed) draw_all();
	}
	cleanup_app();
	return 0;
failed:
	cleanup_app();
	return 1;
}
