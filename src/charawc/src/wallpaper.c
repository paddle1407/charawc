#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

void
chara_wallpaper_adopt(struct wallpaper *wallpaper, struct wallpaper *from)
{
	if (!wallpaper->borrowed)
		return;
	free(wallpaper->pixels);
	wallpaper->pixels = from->pixels;
	wallpaper->borrowed = false;
	from->pixels = NULL;
	from->decoded = false;
}

#ifdef CHARA_NO_PNG

/* Built without libspng. Report success so that a config.lua carrying a
 * wallpaper path still loads: the path is ignored and swc falls back to the
 * solid appearance.wallpaper.background. Failing here would abort the whole
 * configuration, and run.sh checks the configuration before switching VTs,
 * so it would leave the session refusing to start over a decorative setting. */
bool
chara_wallpaper_load(struct wallpaper *wallpaper, const char *path,
                     const struct wallpaper *reuse)
{
	(void)wallpaper;
	(void)reuse;
	_wrn("wallpaper: built without PNG support, ignoring %s; "
	     "using appearance.wallpaper.background", path);
	return true;
}

#else

#include <spng.h>

/* Whether the pixels a previous load decoded came from this same file. */
static bool
same_file(const struct wallpaper *w, const char *path, const struct stat *st)
{
	return w && w->decoded && w->pixels && w->path && !strcmp(w->path, path) &&
	       w->dev == st->st_dev && w->ino == st->st_ino &&
	       w->size == st->st_size &&
	       w->mtim.tv_sec == st->st_mtim.tv_sec &&
	       w->mtim.tv_nsec == st->st_mtim.tv_nsec;
}

static void
note_file(struct wallpaper *w, const struct stat *st)
{
	w->decoded = true;
	w->dev = st->st_dev;
	w->ino = st->st_ino;
	w->size = st->st_size;
	w->mtim = st->st_mtim;
}

bool
chara_wallpaper_load(struct wallpaper *wallpaper, const char *path,
                     const struct wallpaper *reuse)
{
	/* Avoid blocking on FIFOs/devices during a user-triggered reload. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	struct stat st;
	if (fd < 0) {
		_wrn("wallpaper: %s: %s", path, strerror(errno));
		return false;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > 64 * 1024 * 1024) {
		_wrn("wallpaper: expected a regular PNG file of at most 64 MiB: %s", path);
		close(fd);
		return false;
	}
	/* The same image as last time: rather than decoding it all over again,
	 * or copying up to 64 MiB of it, take over the pixels already decoded
	 * once this configuration replaces that one. Until then the running
	 * configuration keeps them, so a reload that fails later loses nothing. */
	if (same_file(reuse, path, &st)) {
		free(wallpaper->pixels);
		wallpaper->pixels = NULL;
		wallpaper->width = reuse->width;
		wallpaper->height = reuse->height;
		wallpaper->borrowed = true;
		note_file(wallpaper, &st);
		close(fd);
		return true;
	}
	FILE *fp = fdopen(fd, "rb");
	if (!fp) { close(fd); return false; }
	spng_ctx *ctx = spng_ctx_new(0);
	unsigned char *pixels = NULL;
	struct spng_ihdr header;
	size_t size;
	int ret = 0;
	bool ok = false;
	if (!ctx) goto done;
	spng_set_image_limits(ctx, 8192, 8192);
	spng_set_chunk_limits(ctx, 8 * 1024 * 1024, 8 * 1024 * 1024);
	if ((ret = spng_set_png_file(ctx, fp)) ||
	    (ret = spng_get_ihdr(ctx, &header)) ||
	    (ret = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size)))
		goto done;
	if (size > 64 * 1024 * 1024) {
		_wrn("wallpaper: decoded PNG exceeds 64 MiB: %s", path);
		goto done;
	}
	if (!(pixels = malloc(size))) goto done;
	if ((ret = spng_decode_image(ctx, pixels, size, SPNG_FMT_RGBA8,
	                            SPNG_DECODE_TRNS | SPNG_DECODE_GAMMA)))
		goto done;
	struct stat after;
	if (fstat(fd, &after) < 0 || after.st_size != st.st_size ||
	    after.st_mtim.tv_sec != st.st_mtim.tv_sec ||
	    after.st_mtim.tv_nsec != st.st_mtim.tv_nsec) {
		_wrn("wallpaper: file changed while reading; retry after saving: %s", path);
		goto done;
	}
	for (size_t i = 0; i < size; i += 4) {
		unsigned char *p = pixels + i;
		uint32_t alpha = p[3], argb;

		/* Photographs are opaque throughout, and premultiplying by one is
		 * three divisions a pixel for nothing. */
		if (alpha == 255)
			argb = 0xff000000u | (uint32_t)p[0] << 16 |
			       (uint32_t)p[1] << 8 | p[2];
		else
			argb = (alpha << 24) |
			    (((p[0] * alpha + 127) / 255) << 16) |
			    (((p[1] * alpha + 127) / 255) << 8) |
			    ((p[2] * alpha + 127) / 255);
		memcpy(p, &argb, sizeof(argb));
	}
	free(wallpaper->pixels);
	wallpaper->pixels = (uint32_t *)pixels;
	wallpaper->width = header.width;
	wallpaper->height = header.height;
	note_file(wallpaper, &st);
	pixels = NULL;
	ok = true;
done:
	if (ret) _wrn("wallpaper: %s: %s", path, spng_strerror(ret));
	free(pixels);
	spng_ctx_free(ctx);
	fclose(fp);
	return ok;
}

#endif /* CHARA_NO_PNG */
