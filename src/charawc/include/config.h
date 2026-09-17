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
bool chara_startup_pending(void);
void chara_child_exited(pid_t);

/* border.c */
struct decor *decor_create(void);
void decor_destroy(struct decor *);
void chara_decorate(struct client *, bool focused);
int32_t chara_border_width(void);                    /* every ring, one side */
int32_t chara_titlebar_height(const struct client *); /* 0 without a titlebar */
void chara_undecorate(struct client *);
void chara_apply_border(struct client *, bool focused);

/* window.c */
void chara_focus(struct client *);
void chara_action_run(const struct action *);
void chara_stop(void);
void chara_request_reload(void);
void chara_minimize(struct client *);
void chara_restore(struct client *);
bool chara_set_fullscreen(struct client *, bool, struct swc_screen *);
bool chara_set_maximized(struct client *, bool);
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
