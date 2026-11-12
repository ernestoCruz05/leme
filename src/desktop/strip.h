#ifndef LEME_DESK_STRIP_H
#define LEME_DESK_STRIP_H

#include "core/leme.h"
#include "desktop/target.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_server;
struct leme_output;
struct leme_render_hit;
struct wlr_pointer_button_event;

bool leme_desk_strip_init(struct leme_server *server);
void leme_desk_strip_finish(struct leme_server *server);
bool leme_desk_strip_sync_output(struct leme_output *output);
void leme_desk_strip_refresh(struct leme_server *server);
void leme_desk_strip_output_finish(struct leme_output *output);
struct leme_box leme_desk_strip_content_box(const struct leme_output *output);
bool leme_desk_strip_icon_box(const struct leme_output *output,
                              struct leme_desk_target target,
                              struct leme_box *out);
size_t leme_desk_strip_capacity(struct leme_box area, size_t count);
struct leme_box leme_desk_strip_area_for_count(struct leme_box area,
                                               size_t count);
bool leme_desk_strip_handle_button(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event);

#endif
