#ifndef CHARA_BAR_SCHEMA_H
#define CHARA_BAR_SCHEMA_H

/*
 * The bounds and option lists for the [bar] config table, shared between
 * charaWC (src/config.c), which validates the whole file and only launches
 * charabar once it passes, and charabar (src/charabar/charabar.c), which
 * re-reads and re-parses the same table itself after every reload. Before
 * this header existed both sides carried their own copy of every range and
 * option list, with nothing to stop them drifting apart: a range charaWC
 * accepted that charabar silently clamped, or the reverse, a config charaWC
 * rejected before charabar ever saw the key.
 *
 * charaWC still keeps only bar.enabled and hashes the rest (see
 * BAR_DIGEST_* in config.c) rather than handing charabar the parsed values:
 * charabar reads config.lua itself, deliberately, so a config that hangs or
 * over-allocates degrades the bar rather than the compositor. Sharing this
 * header is what keeps the two readers agreeing on what a valid value is
 * without also sharing that reader.
 */

#define BAR_HEIGHT_MIN 16
#define BAR_HEIGHT_MAX 128
#define BAR_PADDING_MIN 0
#define BAR_PADDING_MAX 128
#define BAR_SPACING_MIN 0
#define BAR_SPACING_MAX 128
#define BAR_WORKSPACES_COUNT_MIN 1
#define BAR_WORKSPACES_COUNT_MAX 9   /* mirrors CHARA_WORKSPACES in chara.h */
#define BAR_WINDOW_MAX_LENGTH_MIN 1
#define BAR_WINDOW_MAX_LENGTH_MAX 512
#define BAR_TASKBAR_MAX_LENGTH_MIN 1
#define BAR_TASKBAR_MAX_LENGTH_MAX 128
#define BAR_TASKBAR_MIN_WIDTH_MIN 16
#define BAR_TASKBAR_MIN_WIDTH_MAX 512
#define BAR_TASKBAR_MAX_WIDTH_MIN 0
#define BAR_TASKBAR_MAX_WIDTH_MAX 16384
#define BAR_TASKBAR_SCROLL_STEP_MIN 1
#define BAR_TASKBAR_SCROLL_STEP_MAX 1024
/* clock/cpu/memory/network all poll on a timer and would simply freeze at 0. */
#define BAR_TIMED_INTERVAL_MIN 1
#define BAR_TIMED_INTERVAL_MAX 3600
/* The volume module also refreshes on SIGUSR1, so 0 means "never poll". */
#define BAR_VOLUME_INTERVAL_MIN 0
#define BAR_VOLUME_INTERVAL_MAX 3600

/*
 * NULL-terminated, for charaWC's one_of(), which walks a choice list until it
 * hits the sentinel. charabar's lua_enum_field() takes an explicit count
 * instead; BAR_OPTION_COUNT() gives it that count without the sentinel.
 */
#define BAR_OPTION_COUNT(array) (sizeof(array) / sizeof(*(array)) - 1)

static const char *const bar_taskbar_scopes[] = { "workspace", "monitor", "all", NULL };
static const char *const bar_taskbar_overflows[] = { "shrink", "scroll", "none", NULL };

/*
 * The modules a bar section may list, in "modules.left/center/right". charaWC
 * only checks a name is one of these; charabar additionally maps each to the
 * enum it renders, in a table it keeps positionally matched to this one (see
 * the static assert next to module_from_name in charabar.c).
 */
static const char *const bar_module_names[] = {
	"workspaces", "window", "taskbar", "clock", "cpu", "memory", "network",
	"volume", NULL
};

#endif /* CHARA_BAR_SCHEMA_H */
