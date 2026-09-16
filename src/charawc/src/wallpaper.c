#include <errno.h>
#include <fcntl.h>
#include <spng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

bool
chara_wallpaper_load(struct wallpaper *wallpaper, const char *path)
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
		uint32_t alpha = p[3];
		uint32_t argb = (alpha << 24) |
		    (((p[0] * alpha + 127) / 255) << 16) |
		    (((p[1] * alpha + 127) / 255) << 8) |
		    ((p[2] * alpha + 127) / 255);
		memcpy(p, &argb, sizeof(argb));
	}
	free(wallpaper->pixels);
	wallpaper->pixels = (uint32_t *)pixels;
	wallpaper->width = header.width;
	wallpaper->height = header.height;
	pixels = NULL;
	ok = true;
done:
	if (ret) _wrn("wallpaper: %s: %s", path, spng_strerror(ret));
	free(pixels);
	spng_ctx_free(ctx);
	fclose(fp);
	return ok;
}
