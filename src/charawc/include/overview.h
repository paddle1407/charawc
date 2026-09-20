#ifndef CHARA_OVERVIEW_H
#define CHARA_OVERVIEW_H
#include <stdint.h>
#include <swc.h>

/* Pure geometry: no compositor calls or global state. Input order is retained;
 * area sorting uses id to break ties. rect is the image; footer pixels below
 * it are reserved for a label. All images use the returned common scale. */
struct ov_item {
	uint32_t id, src_width, src_height;
	struct swc_rectangle rect;
};
double ov_arrange(struct ov_item *, unsigned n, struct swc_rectangle area,
                  int32_t inner, int32_t outer, uint32_t footer);
#endif
