#ifndef CHARA_TYPES_H
#define CHARA_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

#include <wayland-server.h>
#include <swc.h>

#include "chara.h"
#include "tiling.h"

/* One border ring. rings[0] is the ring touching the window. */
struct ring {
	int32_t  width;
	uint32_t focused, unfocused;
};

struct values {
	uint32_t mod;
	bool     raise_maximized_on_click;
	/* Hovering a window brings it to the front, not just the keyboard. */
	bool     raise_on_hover;
	/* Honor the monitor a client names when it asks to go fullscreen,
	 * instead of filling the monitor its window is already on. */
	bool     fullscreen_follows_client;
	/* Keep the border and the titlebar on screen while maximized. */
	bool     maximize_borders, maximize_titlebar;
	struct ring rings[CHARA_MAX_RINGS];
	unsigned ring_count;
	char     *title_format;
};

struct client {
	struct wl_list    link;       /* wm.clients: creation order */
	struct swc_window *win;
	struct screen     *scr;

	uint32_t id;                       /* auto id, written as #id */
	char     name[CHARA_NAME_MAX];     /* rule-assigned name, or empty */
	unsigned ordinal;                  /* nth window using that name */

	bool visible, fullscreen, maximized, titlebar, movable, resizable;
	bool pinned; /* kept above the other windows */
	uint64_t minimized; /* zero when normal, otherwise most-recent order */

	/* Where the window sits, and how big it is. */
	int32_t  x, y;
	uint32_t width, height;
	uint8_t  ws;

	/*
	 * Tiling.
	 *
	 * `tiled` is membership, and it survives everything: a window that goes
	 * fullscreen, is minimized or is maximized stays a member and keeps its
	 * place, it is only left out of the arrangement while it is like that.
	 * Only the user taking it out, or the window closing, ends it.
	 *
	 * `tile_main` and `tile_cross` are its share of the group it lands in --
	 * see tiling.h -- and they live here, on the window, so that opening and
	 * closing windows cannot scramble the sizes of the ones that stay.
	 */
	bool     tiled;
	/* A rule has spoken about whether this window tiles, so the default in
	 * the configuration must not speak over it. */
	bool     tile_ruled;
	unsigned tile_order;              /* place among its workspace's tiles */
	double   tile_main, tile_cross;
	uint32_t tile_edges;              /* last published to the client */
	bool     tile_mode_set;           /* swc already told it is tiled */
	/* Where it goes when it leaves the tiling, remembered while it is in. */
	struct swc_rectangle floating;
};

struct titlebar_style {
	uint32_t background, foreground;
};

enum titlebar_fullscreen_action {
	TITLEBAR_FULLSCREEN,
	TITLEBAR_MAXIMIZE,
};

struct decor {
	struct swc_decor_parts active, inactive;
	struct swc_titlebar titlebar;
	uint32_t bar_height, bar_padding;
	bool bar_align_set;
	enum swc_decor_align bar_align;
	struct titlebar_style bar_focused, bar_unfocused;
	uint32_t bar_hover, bar_pressed;
	enum titlebar_fullscreen_action fullscreen_action;

	bool enabled;                /* text drawn on a window edge */
	enum swc_decor_edge edge;
	enum swc_decor_align align;
	uint32_t foreground, background;
	uint32_t padding;
	int32_t  offset_x, offset_y;
	char     *fontname;
};

struct rule {
	struct wl_list link;
	char     app_id[CHARA_APP_ID_MAX];
	char     name[CHARA_NAME_MAX];   /* id assigned to matching windows */
	int32_t  x, y;
	uint32_t width, height;
	bool     has_pos, center;
	bool     has_titlebar, titlebar;
	bool     has_tiled, tiled;       /* keep it out of, or in, the tiling */
	bool     has_pinned, pinned;     /* keep it above everything else */
	bool     movable, resizable;
};

struct grab {
	bool          active, resize;
	struct client *client;
};

struct status {
	bool ok;
	char msg[MAXSIZE];
};
typedef struct status status;

struct screen {
	struct wl_list    link;
	struct swc_screen *scr;
	int32_t           x, y;
	uint32_t          width, height;
	/* Every monitor owns its own numbered workspaces, so switching one
	 * leaves the others showing what they were showing. */
	uint8_t           ws;
	/* Focus is remembered per monitor: the active monitor follows the
	 * pointer, so returning to a monitor returns to the window in use
	 * there. Cleared when that window closes or moves away. */
	struct client     *focus;
	/* One layout per workspace, so switching a monitor's workspace switches
	 * the arrangement with it. Index 0 is unused; workspaces count from 1. */
	struct tile_ws    tiles[CHARA_WORKSPACES + 1];
	bool              tile_dirty[CHARA_WORKSPACES + 1];
};

struct wm {
	struct wl_display    *dpy;
	struct wl_event_loop *loop;

	struct wl_list clients, screens, rules;

	struct screen *scr;   /* monitor under the pointer */
	struct client *cur;   /* focused window */
	struct grab   grab;
	uint32_t      last_id;
	uint64_t      minimize_order;

	volatile sig_atomic_t running;
};

#endif /* CHARA_TYPES_H */
