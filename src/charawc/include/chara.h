#ifndef CHARA_H
#define CHARA_H

#include <stdint.h>

#define CHARA_NAME_MAX 32   /* window id names set by rules */
#define CHARA_APP_ID_MAX 64
#define CHARA_MAX_RINGS 2   /* neuswc renders an inner and an outer ring */
#define CHARA_WORKSPACES 9
#define MAXSIZE 4096        /* IPC line buffer */
/* Pointer motion is sampled no faster than this. Resizing a window faster
 * than it can answer only makes it fall further behind. */
#define CHARA_MOTION_THROTTLE_MS (1000 / 85)

/* Logging. _err never returns. */
void _inf(const char *, ...);
void _wrn(const char *, ...);
#if defined(__GNUC__) || defined(__clang__)
#define CHARA_NORETURN __attribute__((noreturn))
#else
#define CHARA_NORETURN
#endif
CHARA_NORETURN void _err(int, const char *, ...);

/* Path of the IPC socket, in XDG_RUNTIME_DIR when available. */
const char *chara_socket_path(void);

#endif /* CHARA_H */
