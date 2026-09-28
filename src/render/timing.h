#ifndef LEME_RENDER_TIMING_H
#define LEME_RENDER_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct leme_render_timing;
struct wlr_scene_timer;

bool leme_render_timing_parse(const char *value, uint32_t *seconds);
struct leme_render_timing *leme_render_timing_create(uint32_t interval_seconds);
void leme_render_timing_destroy(struct leme_render_timing *timing);
struct wlr_scene_timer *
leme_render_timing_begin(struct leme_render_timing *timing, const char *name);
void leme_render_timing_end(struct leme_render_timing *timing,
                            const struct timespec *start, bool rendered);

#endif
