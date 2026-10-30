#ifndef LEME_DESK_DRAG_H
#define LEME_DESK_DRAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_server;
struct wlr_scene_tree;

bool leme_desk_drag_begin(struct leme_server *server, uint32_t time,
                          char *const *paths, size_t count,
                          struct wlr_scene_tree *image);
bool leme_desk_drag_active(const struct leme_server *server);
void leme_desk_drag_motion(struct leme_server *server);
void leme_desk_drag_cancel(struct leme_server *server);
void leme_desk_drag_finish(struct leme_server *server);

#endif
