#ifndef CHARA_WALLPAPER_H
#define CHARA_WALLPAPER_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <swc.h>

struct wallpaper {
	char *path;
	uint32_t *pixels;
	uint32_t width, height;
	enum swc_wallpaper_mode mode;
	uint32_t background;
	/* Which file the pixels came from. A reload decodes the same image
	 * again otherwise, which is the slowest thing it does. */
	bool decoded;
	dev_t dev;
	ino_t ino;
	off_t size;
	struct timespec mtim;
	/* The same image as the running configuration's: pixels is left empty
	 * and chara_wallpaper_adopt takes that one's once this one is in use. */
	bool borrowed;
};

/* Decode a regular PNG into owned, premultiplied ARGB8888 pixels. When reuse
 * names the same file, nothing is decoded or copied: the result is marked
 * borrowed and gets reuse's pixels from chara_wallpaper_adopt. */
bool chara_wallpaper_load(struct wallpaper *, const char *path,
                          const struct wallpaper *reuse);
/* Hand a borrowed wallpaper the pixels it named. `from` gives them up. */
void chara_wallpaper_adopt(struct wallpaper *, struct wallpaper *from);

#endif
