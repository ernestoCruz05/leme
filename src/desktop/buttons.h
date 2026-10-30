#ifndef LEME_DESK_BUTTONS_H
#define LEME_DESK_BUTTONS_H

#include "render/render.h"

struct wlr_pointer_button_event;

bool leme_desk_buttons_available(const struct leme_view *view,
                                 enum leme_render_decoration_part part);
bool leme_desk_buttons_handle_button(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event);
void leme_desk_buttons_handle_motion(struct leme_server *server,
                                     const struct leme_render_hit *hit);
void leme_desk_buttons_cancel(struct leme_server *server);
void leme_desk_buttons_invalidate_view(struct leme_view *view);
void leme_desk_buttons_invalidate_container(struct leme_container *container);
void leme_desk_buttons_refresh(struct leme_server *server);
void leme_desk_buttons_finish(struct leme_server *server);

#endif
