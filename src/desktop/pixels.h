#ifndef LEME_DESK_PIXELS_H
#define LEME_DESK_PIXELS_H

#include "core/leme.h"

#include <stdbool.h>

enum leme_desk_control_icon {
  LEME_DESK_CONTROL_MINIMIZE,
  LEME_DESK_CONTROL_MAXIMIZE,
  LEME_DESK_CONTROL_RESTORE,
  LEME_DESK_CONTROL_CLOSE,
};

struct wlr_buffer;
struct wlr_scene_tree;

bool leme_desk_pixels_rect(struct wlr_scene_tree *tree, struct leme_box box,
                           const float color[4], int radius, bool ring);
bool leme_desk_pixels_place_line(struct wlr_scene_tree *tree, const char *text,
                                 const char *font, const float color[4],
                                 double scale, struct leme_box box,
                                 bool center);

bool leme_desk_pixels_place_buffer(struct wlr_scene_tree *tree,
                                   struct wlr_buffer *buffer,
                                   struct leme_box box);

struct wlr_buffer *leme_desk_pixels_load(const char *path, int pixels);
struct wlr_buffer *leme_desk_pixels_label(const char *text, const char *font,
                                          const float color[4], bool shadow,
                                          int logical_width, double scale,
                                          int *logical_height);

struct wlr_buffer *leme_desk_pixels_line(const char *text, const char *font,
                                         const float color[4],
                                         int logical_width, double scale,
                                         int *fitted_width,
                                         int *logical_height);

struct wlr_buffer *leme_desk_pixels_entry(const char *text, const char *font,
                                          const float color[4],
                                          int logical_width, double scale,
                                          size_t cursor, int *logical_height);

struct wlr_buffer *leme_desk_pixels_control(enum leme_desk_control_icon icon,
                                            const float color[4], int size,
                                            double scale);

void leme_desk_utf8_copy(char *out, size_t capacity, const char *text);

#endif
