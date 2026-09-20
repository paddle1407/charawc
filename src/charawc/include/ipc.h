#ifndef CHARA_IPC_H
#define CHARA_IPC_H

/* Every command is reachable from charactl and from a Lua key binding.
 * Commands taking a window act on the selector given as their first
 * argument, defaulting to the focused window. */
enum cmd {
	/* geometry */
	cmd_move, cmd_move_absolute, cmd_resize, cmd_resize_absolute,
	cmd_teleport, cmd_center,
	/* state */
	cmd_fullscreen, cmd_maximize, cmd_minimize, cmd_restore,
	cmd_hide, cmd_show, cmd_raise, cmd_lower, cmd_pin, cmd_close,
	/* focus */
	cmd_focus, cmd_focus_next, cmd_focus_prev, cmd_unfocus,
	/* workspaces */
	cmd_workspace, cmd_move_workspace,
	/*
	 * Tiling, and the directional focus that comes with it. The directions
	 * are separate commands rather than one command taking a direction
	 * because an action carries numbers and a window selector and nothing
	 * else, and `focus_left` reads better in a key binding than a direction
	 * spelled as a number would.
	 *
	 * focus_* is not tiling-only: it asks which window lies that way on the
	 * screen, which a floating window answers as well as a tiled one.
	 */
	cmd_tile, cmd_tile_workspace, cmd_tile_promote, cmd_tile_swap,
	cmd_tile_equalize,
	cmd_focus_left, cmd_focus_right, cmd_focus_up, cmd_focus_down,
	cmd_tile_move_left, cmd_tile_move_right,
	cmd_tile_move_up, cmd_tile_move_down,
	cmd_tile_resize_left, cmd_tile_resize_right,
	cmd_tile_resize_up, cmd_tile_resize_down,
	cmd_tile_master, cmd_tile_columns, cmd_tile_rows,
	cmd_tile_grid, cmd_tile_monocle,
	cmd_tile_layout_next, cmd_tile_layout_prev,
	cmd_tile_master_count, cmd_tile_master_ratio,
	/*
	 * Screen. Zoom scales the whole monitor about its centre; it is a
	 * magnifier, so what is under the pointer does not move with it.
	 */
	cmd_zoom, cmd_overview,
	/* queries */
	cmd_get_geometry, cmd_get_pid, cmd_get_title, cmd_get_app_id,
	cmd_get_id, cmd_get_focus, cmd_get_workspace, cmd_get_tiling,
	cmd_get_screen_geometry, cmd_get_cursor_position,
	cmd_list_windows, cmd_list_monitors,
	/* session */
	cmd_reload, cmd_quit,
	cmd_last
};

struct command {
	const char *name;
	enum cmd command;
	int argc;          /* required arguments after an optional selector */
	bool selects;      /* takes a leading window selector */
	const char *usage;
	/* Arguments beyond the required ones that may be given. The resize
	 * commands use it for the step, so that a key binding can name one and
	 * leaving it out means the one in the configuration. */
	int optional;
};

extern const struct command commands[cmd_last];

#endif /* CHARA_IPC_H */
