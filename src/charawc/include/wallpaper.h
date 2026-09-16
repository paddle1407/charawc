#ifndef CHARA_WALLPAPER_H
#define CHARA_WALLPAPER_H

#include <swc.h>

struct wallpaper {
	char *path;
	uint32_t *pixels;
	uint32_t width, height;
	enum swc_wallpaper_mode mode;
	uint32_t background;
};

/* Decode a regular PNG into owned, premultiplied ARGB8888 pixels. */
bool chara_wallpaper_load(struct wallpaper *, const char *path);

#endif
