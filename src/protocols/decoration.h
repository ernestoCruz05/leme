#ifndef LEME_DECORATION_PROTOCOL_H
#define LEME_DECORATION_PROTOCOL_H

#include "core/leme.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_container;
struct leme_server;
struct leme_view;
struct wlr_scene_tree;

bool leme_decoration_protocol_init(struct leme_server *server);
void leme_decoration_protocol_finish(struct leme_server *server);
void leme_decoration_protocol_schedule(struct leme_server *server);
void leme_decoration_protocol_view_destroy(struct leme_view *view);
void leme_decoration_protocol_view_arrange(struct leme_view *view,
                                           int content_x, int content_y);
void leme_decoration_protocol_view_present(struct leme_view *view);
bool leme_view_has_shell_decoration(const struct leme_view *view,
                                    bool fullscreen);
struct leme_gaps leme_decoration_protocol_insets(const struct leme_view *view,
                                                 bool fullscreen);
bool leme_decoration_protocol_replaces_border(const struct leme_view *view,
                                              bool fullscreen);
bool leme_decoration_protocol_holds_open(const struct leme_view *view);
size_t leme_decoration_protocol_trees(const struct leme_view *view,
                                      struct wlr_scene_tree **trees,
                                      size_t capacity);
bool leme_decoration_protocol_container_decorated(
    const struct leme_container *container);
bool leme_decoration_protocol_container_replaces_border(
    const struct leme_container *container);
struct leme_gaps leme_decoration_protocol_container_insets(
    const struct leme_container *container);
void leme_decoration_protocol_container_arrange(
    struct leme_container *container, int content_x, int content_y);
size_t
leme_decoration_protocol_container_trees(const struct leme_container *container,
                                         struct wlr_scene_tree **trees,
                                         size_t capacity);
void leme_decoration_protocol_container_destroy(
    struct leme_container *container);

#endif
