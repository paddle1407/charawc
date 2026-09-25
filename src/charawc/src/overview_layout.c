#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "overview.h"

struct entry { struct ov_item item; unsigned index; };
static int order(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	uint64_t xa = (uint64_t)x->item.src_width * x->item.src_height;
	uint64_t ya = (uint64_t)y->item.src_width * y->item.src_height;
	if (xa != ya) return xa > ya ? -1 : 1;
	return (x->item.id > y->item.id) - (x->item.id < y->item.id);
}

static bool fits(struct swc_rectangle r, const struct swc_rectangle *placed,
                 unsigned n, int64_t width, int64_t height, int gap, unsigned footer)
{
	if ((int64_t)r.x + r.width > width || (int64_t)r.y + r.height + footer > height)
		return false;
	for (unsigned i = 0; i < n; ++i) {
		struct swc_rectangle p = placed[i];
		if ((int64_t)r.x < (int64_t)p.x + p.width + gap &&
		    (int64_t)p.x < (int64_t)r.x + r.width + gap &&
		    (int64_t)r.y < (int64_t)p.y + p.height + footer + gap &&
		    (int64_t)p.y < (int64_t)r.y + r.height + footer + gap)
			return false;
	}
	return true;
}

static bool pack(const struct entry *e, unsigned n, struct swc_rectangle *out,
                 int64_t width, int64_t height, int gap, unsigned footer, double scale)
{
	for (unsigned i = 0; i < n; ++i) {
		struct swc_rectangle r = {0, 0,
			(uint32_t)fmax(1, floor(e[i].item.src_width * scale + .5)),
			(uint32_t)fmax(1, floor(e[i].item.src_height * scale + .5))};
		bool found = false;
		struct swc_rectangle best = r;
		/* Origin and the two exposed corners of every placed rectangle. */
		for (unsigned k = 0; k <= 2 * i; ++k) {
			r.x = r.y = 0;
			if (k) {
				struct swc_rectangle p = out[(k - 1) / 2];
				r.x = p.x; r.y = p.y;
				if (k & 1) r.x += p.width + gap;
				else r.y += p.height + footer + gap;
			}
			if ((!found || r.y < best.y || (r.y == best.y && r.x < best.x)) &&
			    fits(r, out, i, width, height, gap, footer)) {
				best = r; found = true;
			}
		}
		if (!found) return false;
		out[i] = best;
	}
	return true;
}

double ov_arrange(struct ov_item *items, unsigned n, struct swc_rectangle area,
                  int32_t inner, int32_t outer, uint32_t footer)
{
	if (!n) return 1;
	if (!items || inner < 0 || outer < 0 || area.width > INT_MAX / 2 ||
	    area.height > INT_MAX / 2) return 0;
	int64_t width = (int64_t)area.width - 2LL * outer;
	int64_t height = (int64_t)area.height - 2LL * outer;
	if (width < 1 || height <= footer ||
	    (int64_t)area.x + area.width > INT_MAX ||
	    (int64_t)area.y + area.height > INT_MAX) return 0;
	struct entry *e = calloc(n, sizeof(*e));
	/* Two buffers: `out` always holds the packing for `low`, the best scale
	 * known to fit, and trials go into the other, so the answer never has to
	 * be packed a second time once it is found. */
	struct swc_rectangle *buffers = calloc(2 * (size_t)n, sizeof(*buffers));
	struct swc_rectangle *out = buffers, *trial = buffers ? buffers + n : NULL;
	uint32_t largest = 1;
	if (!e || !buffers) { free(e); free(buffers); return 0; }
	for (unsigned i = 0; i < n; ++i) {
		if (!items[i].src_width || !items[i].src_height) { free(e); free(buffers); return 0; }
		e[i] = (struct entry){ items[i], i };
		if (items[i].src_width > largest) largest = items[i].src_width;
		if (items[i].src_height > largest) largest = items[i].src_height;
	}
	qsort(e, n, sizeof(*e), order);
	if (!pack(e, n, out, width, height, inner, footer, 0)) {
		free(e); free(buffers); return 0;
	}
	/* Each trial is a full packing, cubic in n. Past the point where the
	 * interval moves the largest image by a sixty-fourth of a pixel, halving
	 * it again changes no size that rounding can show, so stop there rather
	 * than after a fixed fifty halvings; the cap keeps the old bound. */
	double low = 0, high = 1;
	for (unsigned k = 0; k < 50 && (high - low) * largest > 1.0 / 64; ++k) {
		double mid = (low + high) / 2;
		if (pack(e, n, trial, width, height, inner, footer, mid)) {
			struct swc_rectangle *swap = out;
			out = trial;
			trial = swap;
			low = mid;
		} else {
			high = mid;
		}
	}
	int64_t right = 0, bottom = 0;
	for (unsigned i = 0; i < n; ++i) {
		if (out[i].x + out[i].width > right) right = out[i].x + out[i].width;
		if (out[i].y + out[i].height + footer > bottom) bottom = out[i].y + out[i].height + footer;
	}
	for (unsigned i = 0; i < n; ++i) {
		out[i].x += area.x + outer + (width - right) / 2;
		out[i].y += area.y + outer + (height - bottom) / 2;
		items[e[i].index].rect = out[i];
	}
	free(e); free(buffers);
	return low;
}
