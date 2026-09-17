#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

struct decor *
decor_create(void)
{
	struct decor *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;

	d->fontname = strdup("sans-serif:size=11");
	if (!d->fontname) {
		free(d);
		return NULL;
	}
	d->edge = SWC_DECOR_EDGE_TOP;
	d->align = SWC_DECOR_ALIGN_START;
	d->foreground = 0xffebdbb2;
	d->background = 0xff1d2021;
	d->bar_height = 24;
	d->bar_padding = 8;
	d->bar_align = SWC_DECOR_ALIGN_CENTER;
	d->bar_focused = (struct titlebar_style){ 0xff1d2021, 0xffebdbb2 };
	d->bar_unfocused = (struct titlebar_style){ 0xff141617, 0xff928374 };
	d->titlebar.buttons_style = SWC_TITLEBAR_BUTTONS_CLASSIC;
	d->titlebar.count = 3;
	d->titlebar.buttons[0] = SWC_TITLEBAR_MINIMIZE;
	d->titlebar.buttons[1] = SWC_TITLEBAR_FULLSCREEN;
	d->titlebar.buttons[2] = SWC_TITLEBAR_CLOSE;
	d->fullscreen_action = TITLEBAR_MAXIMIZE;
	return d;
}

void
decor_destroy(struct decor *d)
{
	if (!d)
		return;
	free(d->fontname);
	free(d);
}

/* Expand the configured title format for one window.
 * %t title, %a app_id, %i window id, %w workspace, %% a literal percent. */
static const char *
expand_title(struct client *c)
{
	static char buf[MAXSIZE];
	const char *fmt = config.values.title_format;
	size_t o = 0;

	if (!fmt || !c || !c->win) {
		buf[0] = '\0';
		return buf;
	}
	for (const char *p = fmt; *p && o + 1 < sizeof(buf); ++p) {
		char label[CHARA_NAME_MAX + 16];
		const char *insert = NULL;
		char number[16];

		if (*p != '%') {
			buf[o++] = *p;
			continue;
		}
		switch (*++p) {
		case 't': insert = c->win->title; break;
		case 'a': insert = c->win->app_id; break;
		case 'i':
			chara_client_label(c, label, sizeof(label));
			insert = label;
			break;
		case 'w':
			snprintf(number, sizeof(number), "%u", c->ws);
			insert = number;
			break;
		case '%': insert = "%"; break;
		case '\0': --p; continue;
		default: continue;
		}
		if (!insert)
			continue;
		size_t n = strlen(insert);
		if (n > sizeof(buf) - 1 - o)
			n = sizeof(buf) - 1 - o;
		memcpy(buf + o, insert, n);
		o += n;
	}
	buf[o] = '\0';
	return buf;
}

/* Borders and titlebars are drawn outside the window's content geometry, so
 * whoever positions a window has to leave room for them. */
int32_t
chara_border_width(void)
{
	int32_t total = 0;
	for (unsigned i = 0; i < config.values.ring_count; ++i)
		total += config.values.rings[i].width;
	return total;
}

static bool
titlebar_shown(const struct client *c)
{
	return config.decoration->titlebar.enabled && c->titlebar && !c->fullscreen;
}

int32_t
chara_titlebar_height(const struct client *c)
{
	return titlebar_shown(c) ? (int32_t)config.decoration->bar_height : 0;
}

void
chara_apply_border(struct client *c, bool focused)
{
	const struct ring *inner = &config.values.rings[0];
	const struct ring *outer = config.values.ring_count > 1
	    ? &config.values.rings[1] : NULL;

	swc_window_set_border(c->win,
	    focused ? inner->focused : inner->unfocused,
	    config.values.ring_count ? inner->width : 0,
	    outer ? (focused ? outer->focused : outer->unfocused) : 0,
	    outer ? outer->width : 0);
}

void
chara_decorate(struct client *c, bool focused)
{
	struct decor *d = config.decoration;
	struct swc_rectangle geometry;

	chara_apply_border(c, focused);

	bool bar = titlebar_shown(c);
	if (!d->enabled && !bar) {
		swc_window_set_decor(c->win, NULL);
		return;
	}

	const struct titlebar_style *style = focused ? &d->bar_focused
	                                             : &d->bar_unfocused;
	struct swc_decor decor = {
		.color = bar ? style->background : d->background,
		.top = bar ? d->bar_height : 0,
		.parts = NULL,
		.title = {
			.enabled = d->enabled || bar,
			.edge = bar ? SWC_DECOR_EDGE_TOP : d->edge,
			.align = bar && d->bar_align_set ? d->bar_align : d->align,
			.string = expand_title(c),
			.color = bar ? style->foreground : d->foreground,
			.padding = bar ? d->bar_padding : d->padding,
			.offset_x = bar ? 0 : d->offset_x,
			.offset_y = bar ? 0 : d->offset_y,
			.font = d->fontname,
		},
		.titlebar = d->titlebar,
	};
	decor.titlebar.enabled = bar;
	decor.titlebar.pinned = c->pinned;
	decor.titlebar.hover_color = d->bar_hover;
	decor.titlebar.pressed_color = d->bar_pressed;

	/* Prepared decor cannot fail to apply, so a failed allocation leaves the
	 * window with the decoration it already had. */
	if (!swc_window_get_geometry(c->win, &geometry))
		geometry.width = c->width;
	struct swc_prepared_decor *prepared = swc_decor_prepare(&decor, geometry.width);
	if (prepared)
		swc_window_apply_decor(c->win, prepared);
}

void
chara_undecorate(struct client *c)
{
	swc_window_set_decor(c->win, NULL);
	swc_window_set_border(c->win, 0, 0, 0, 0);
}
