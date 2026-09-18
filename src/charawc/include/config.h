#ifndef CHARA_CONFIG_H
#define CHARA_CONFIG_H

#include <sys/types.h>

#include "types.h"
#include "ipc.h"
#include "wallpaper.h"

struct action {
	enum cmd command;
	unsigned argc;
	int32_t args[4];
	char *selector;   /* optional window selector, or NULL */
};

struct binding {
	struct wl_list link;
	uint32_t modifiers, key;
	char **argv;        /* spawn, or NULL when action is used */
	struct action action;
};

struct startup_command {
	struct wl_list link;
	char **argv;
	char *ready_socket;
	bool wait, stop_on_exit;
	int timeout_ms;
};

struct monitor_config {
	struct wl_list link;
	char *name;
	int32_t x, y;
	bool matched;
};

/* Where a newly tiled window lands in its workspace's order. */
enum tile_insert {
	TILE_INSERT_AFTER_FOCUS,
	TILE_INSERT_END,
	TILE_INSERT_START,
};

struct tiling_config {
	/* Whether a workspace starts out tiling. Turning it on and off at
	 * runtime is per workspace -- see `tile_workspace` -- and is not
	 * written back here. */
	bool     enabled;
	/* The layout a workspace starts on. Changing it at runtime is per
	 * workspace and is not written back here. */
	enum tile_layout layout;
	enum tile_side   master_side;
	double   master_ratio;
	unsigned master_count;
	int32_t  inner_gap, outer_gap;
	bool     smart_gaps;      /* a workspace with one window gets none */
	int32_t  resize_step;     /* pixels a keyboard resize moves a fence */
	enum tile_insert insert;
	/* Arranging slides windows under a still pointer, and the enter that
	 * follows is not the user pointing at anything. Off by default. */
	bool     focus_follows_relayout;
	/* Dropping a dragged window on a tiled one trades their places. */
	bool     drag_swaps;
};

struct bar_config {
	bool enabled;
	/* Digest of the whole bar table. charabar reads config.lua only when it
	 * starts, so charaWC restarts it whenever this changes -- including for
	 * the settings charaWC merely validates and never keeps. */
	uint64_t digest;
};

struct config {
	struct values values;
	struct decor *decoration;
	struct wl_list bindings, rules, exec_once, exec, monitors;
	char *cursor_theme;
	int cursor_size;
	struct wallpaper wallpaper;
	struct bar_config bar;
	struct tiling_config tiling;
};

/* config.c */
bool chara_config_init(struct config *);
bool chara_config_load(struct config *, const char *path);
void chara_config_finish(struct config *);
/* Hands src's contents to dst and leaves src empty. struct config embeds
 * intrusive wl_list heads, whose elements point back at the head's own
 * address, so a configuration cannot be relocated by assignment. */
void chara_config_move(struct config *dst, struct config *src);
/* Writes the starter configuration, but never over an existing file. */
bool chara_config_write_example(const char *path);
bool chara_config_bindings(struct config *);
void chara_config_start(struct config *);
char *chara_config_path(const char *filename);

/* bindings.c */
bool chara_parse_key(const char *, uint32_t mod, uint32_t *mods, uint32_t *key,
                     bool modifiers_only);
bool chara_binding_install(struct binding *); /* takes ownership on success */
bool chara_binding_remove(uint32_t mods, uint32_t key);
void chara_binding_free(struct binding *);
void chara_bindings_finish(void); /* after swc_finalize */
bool chara_bindings_prepare(struct config *, struct swc_binding_batch *);
void chara_bindings_replace(struct config *);
void chara_argv_free(char **);
char **chara_argv_copy(char *const *);
void chara_spawn(char *const *);
pid_t chara_spawn_process(char *const *, bool stop_on_exit);

/* startup.c */
void chara_startup_command_free(struct startup_command *);
bool chara_startup_init(struct wl_event_loop *);
void chara_startup_finish(void);
void chara_startup_run(struct config *);
void chara_child_exited(pid_t);

/* tiling.c -- the glue between the layout engine and the compositor. The
 * geometry itself is in tiling.h, and is pure. */
bool chara_tiling_init(struct wl_event_loop *);
void chara_tiling_finish(void);
/* Ask for a workspace to be laid out again. Cheap and idempotent: the work
 * happens once, at the end of the event loop turn, however often it is asked
 * for, so a burst of events costs one configure per window. */
void chara_tiling_dirty(struct screen *, uint8_t ws);
void chara_tiling_dirty_client(const struct client *);
void chara_tiling_dirty_all(void);
void chara_tiling_flush(void);            /* lay out everything pending, now */
void chara_tiling_ws_reset(struct screen *); /* layouts back to the config */
/* A new monitor: the layouts, and whether each workspace tiles. */
void chara_tiling_ws_init(struct screen *);
/* Whether windows opening on that workspace join its tiling. */
bool chara_tiling_ws_enabled(const struct screen *, uint8_t ws);
/* Turn a workspace's tiling on or off, taking the windows already on it in
 * or out with it. Windows a rule has spoken for are left alone. */
void chara_tiling_ws_enable(struct screen *, uint8_t ws, bool on);
/* True when the enter that just arrived was a window sliding under a pointer
 * that never moved, rather than the user pointing at something. */
bool chara_tiling_ignore_enter(void);
/* Whether focusing this window must raise it too, as monocle needs. */
bool chara_tiling_focus_raises(const struct client *);

void chara_tiling_admit(struct client *);   /* place a new window */
void chara_tiling_forget(struct client *);  /* it leaves the tiling for good */
bool chara_tiling_set(struct client *, bool tiled);
/* Out of the tiling, but left exactly where it is: what a drag wants. */
bool chara_tiling_release_in_place(struct client *);
/* After a fullscreen or a maximize handed the window back to the layout. */
void chara_tiling_restore_mode(struct client *);
/* After a window changed monitor or workspace. */
void chara_tiling_reseat(struct client *, struct screen *from, uint8_t from_ws);

/* Focus the window lying in a direction, on this monitor or the next one.
 * Works over floating windows too: it asks about the screen, not the layout. */
bool chara_focus_dir(enum tile_dir);
bool chara_tiling_move_dir(struct client *, enum tile_dir);
bool chara_tiling_resize_dir(struct client *, enum tile_dir, int32_t pixels);
bool chara_tiling_swap(struct client *, struct client *);
bool chara_tiling_promote(struct client *);
bool chara_tiling_drop_at(struct client *, int32_t x, int32_t y);
struct client *chara_tiling_at(int32_t x, int32_t y, struct swc_rectangle *cell);
void chara_tiling_set_layout(struct screen *, uint8_t ws, enum tile_layout);
void chara_tiling_cycle_layout(struct screen *, uint8_t ws, int direction);
bool chara_tiling_master_count(struct screen *, uint8_t ws, int32_t delta);
bool chara_tiling_master_ratio(struct screen *, uint8_t ws, int32_t percent);
void chara_tiling_equalize(struct screen *, uint8_t ws);
struct tile_ws *chara_tiling_ws(struct screen *, uint8_t ws);
unsigned chara_tiling_count(struct screen *, uint8_t ws);

/* border.c */
struct swc_rectangle chara_frame_inset(const struct client *,
                                       struct swc_rectangle cell);
struct swc_rectangle chara_frame_inset_by(struct swc_rectangle cell,
                                          int32_t side, int32_t top);
struct decor *decor_create(void);
void decor_destroy(struct decor *);
void chara_decorate(struct client *, bool focused);
int32_t chara_border_width(void);                    /* every ring, one side */
int32_t chara_titlebar_height(const struct client *); /* 0 without a titlebar */
void chara_undecorate(struct client *);
void chara_apply_border(struct client *, bool focused);

/* window.c */
void chara_focus(struct client *);
void chara_forget_focus(const struct client *, const struct screen *keep);
void chara_action_run(const struct action *);
void chara_stop(void);
void chara_request_reload(void);
void chara_minimize(struct client *);
void chara_restore(struct client *);
bool chara_set_fullscreen(struct client *, bool, struct swc_screen *);
bool chara_set_maximized(struct client *, bool);
bool chara_set_pinned(struct client *, bool);
void chara_update_mode_geometry(struct client *);
void chara_window_changed(struct client *); /* after an interactive drag */
struct screen *chara_window_screen(const struct client *);
struct screen *chara_active_screen(void);
uint8_t chara_active_ws(void);
struct client *chara_lookup(const char *selector);
void chara_client_label(const struct client *, char *out, size_t size);
void chara_sync_windows(void);
void chara_ws_go_to(struct screen *, uint8_t);
void chara_ws_move_to(uint8_t, struct client *);
void chara_focus_step(int direction);
struct screen *chara_screen_at(int32_t x, int32_t y);

/* charawc.c */
void chara_new_screen(struct swc_screen *);
void chara_bind_mouse(uint32_t mod);
struct screen *chara_screen_of(struct swc_screen *);

/* window.c */
void chara_new_window(struct swc_window *);

/* ipc.c */
bool chara_ipc_init(struct wl_event_loop *);
void chara_ipc_finish(void);
status chara_ipc_dispatch(const struct command *, int argc, char **argv);

extern struct wm wm;
extern struct config config;

#endif
