#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "overview.h"

static unsigned checks;
static void verify(struct ov_item *items, unsigned n, struct swc_rectangle area,
                   unsigned gap, unsigned outer, unsigned footer)
{
	struct ov_item repeat[128], reverse[128];
	memcpy(repeat, items, n * sizeof(*items));
	for (unsigned i = 0; i < n; ++i) reverse[n - i - 1] = items[i];
	double s = ov_arrange(items, n, area, gap, outer, footer);
	assert(s > 0 && s <= 1);
	assert(ov_arrange(repeat, n, area, gap, outer, footer) == s);
	assert(ov_arrange(reverse, n, area, gap, outer, footer) == s);
	for (unsigned i = 0; i < n; ++i) {
		struct swc_rectangle a = items[i].rect;
		assert(!memcmp(&a, &repeat[i].rect, sizeof(a)));
		assert(!memcmp(&a, &reverse[n-i-1].rect, sizeof(a)));
		assert(a.width == (uint32_t)fmax(1, floor(items[i].src_width * s + .5)));
		assert(a.height == (uint32_t)fmax(1, floor(items[i].src_height * s + .5)));
		assert(a.x >= area.x + (int)outer && a.y >= area.y + (int)outer);
		assert((int64_t)a.x + a.width <= (int64_t)area.x + area.width - outer);
		assert((int64_t)a.y + a.height + footer <= (int64_t)area.y + area.height - outer);
		for (unsigned j = 0; j < i; ++j) {
			struct swc_rectangle b = items[j].rect;
			assert((int64_t)a.x + a.width + gap <= b.x ||
			       (int64_t)b.x + b.width + gap <= a.x ||
			       (int64_t)a.y + a.height + footer + gap <= b.y ||
			       (int64_t)b.y + b.height + footer + gap <= a.y);
		}
	}
	++checks;
}
int main(void)
{
	struct ov_item items[128];
	struct swc_rectangle area = {-1920, -200, 1920, 1050};
	assert(ov_arrange(NULL, 0, area, 8, 30, 24) == 1);
	uint32_t seed = 713;
	for (unsigned n = 1; n <= 64; ++n) {
		for (unsigned i = 0; i < n; ++i) {
			seed = seed * 1664525 + 1013904223;
			items[i] = (struct ov_item){ .id = i+1, .src_width = 80 + seed % 2000,
			    .src_height = 80 + (seed >> 12) % 1500 };
		}
		items[0].src_width = 3200; items[0].src_height = 900;
		if (n > 1) { items[1].src_width = 900; items[1].src_height = 3200; }
		verify(items, n, area, 8, 30, 24);
		verify(items, n, (struct swc_rectangle){1920, 30, 1280, 720}, 5, 20, 0);
	}
	for (unsigned i = 0; i < 128; ++i)
		items[i] = (struct ov_item){ .id=i+1, .src_width=1920, .src_height=1080 };
	verify(items, 128, area, 8, 30, 24);
	assert(ov_arrange(items, 1, (struct swc_rectangle){0,0,10,10}, 8, 30, 24) == 0);
	items[0].src_width = 0;
	assert(ov_arrange(items, 1, area, 8, 30, 0) == 0);
	printf("overview layout: %u packing scenarios passed\n", checks);
	return 0;
}
