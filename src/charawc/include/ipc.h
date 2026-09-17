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
	/* queries */
	cmd_get_geometry, cmd_get_pid, cmd_get_title, cmd_get_app_id,
	cmd_get_id, cmd_get_focus, cmd_get_workspace,
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
};

extern const struct command commands[cmd_last];

#endif /* CHARA_IPC_H */
