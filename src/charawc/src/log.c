#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <swc.h>
#include <wayland-server.h>

#include "chara.h"
#include "types.h"

extern struct wm wm;

static void
emit(FILE *out, const char *prefix, const char *msg, va_list list)
{
	fputs(prefix, out);
	vfprintf(out, msg, list);
	fputc('\n', out);
	fflush(out);
}

void
_inf(const char *msg, ...)
{
	va_list list;
	va_start(list, msg);
	emit(stdout, "\033[92mINFO\033[0m: ", msg, list);
	va_end(list);
}

void
_wrn(const char *msg, ...)
{
	va_list list;
	va_start(list, msg);
	emit(stderr, "\033[93mWARN\033[0m: ", msg, list);
	va_end(list);
}

CHARA_NORETURN void
_err(int code, const char *msg, ...)
{
	va_list list;
	va_start(list, msg);
	emit(stderr, "\033[91mFATAL\033[0m: ", msg, list);
	va_end(list);

	wm.running = false;
	if (wm.dpy) {
		swc_finalize();
		wl_display_terminate(wm.dpy);
	}
	exit(code);
}
