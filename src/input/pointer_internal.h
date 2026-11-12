#ifndef LEME_INPUT_POINTER_INTERNAL_H
#define LEME_INPUT_POINTER_INTERNAL_H

#include "config/config.h"
#include "core/leme.h"
#include "public/identity.h"
#include "render/render.h"
#include "shell/container.h"
#include "shell/placement.h"
#include "workspace/layout.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

struct leme_edge_reader;
struct leme_output;
struct leme_server;
struct leme_view;
struct wlr_input_device;
struct wlr_pointer_button_event;

struct leme_pointer {
  struct leme_server *server;
  struct wlr_input_device *device;
  struct leme_public_id public_id;
  double scroll_factor;
  struct leme_edge_reader *edge;
  struct wl_listener destroy;
  struct wl_list link;
};

enum leme_pointer_grab_mode {
  LEME_POINTER_GRAB_NONE,
  LEME_POINTER_GRAB_FLOAT_MOVE,
  LEME_POINTER_GRAB_FLOAT_RESIZE,
  LEME_POINTER_GRAB_CONTAINER_MOVE,
  LEME_POINTER_GRAB_CONTAINER_RESIZE,
  LEME_POINTER_GRAB_CONTAINER_DETACH,
  LEME_POINTER_GRAB_CONTAINER_SPLIT,
  LEME_POINTER_GRAB_TILED_MOVE,
  LEME_POINTER_GRAB_TILED_RESIZE,
  LEME_POINTER_GRAB_SINK,
};

struct leme_pointer_grab {
  struct leme_server *server;
  struct wlr_seat_pointer_grab seat_grab;
  enum leme_pointer_grab_mode mode;
  struct leme_view *view;
  struct leme_public_id container;
  struct leme_public_id source_container;
  struct leme_public_id source_survivor;
  uint64_t source_generation;
  struct leme_box source_content;
  uint32_t button;
  double cursor_x;
  double cursor_y;
  struct leme_box saved_box;
  enum leme_grab_edge edges;
  enum leme_grab_edge hover_edges;
  struct leme_layout_detach *detach;
  struct leme_layout_resize_drag resize_drag;
  struct leme_view *drop_target;
  enum leme_direction drop_direction;
  bool preview_failed;
  struct leme_output *destination;
  bool drop_empty;
  bool restore_pending;
  bool titlebar_pending;
  bool released;
  bool snap_enabled;
  enum leme_view_placement snap_placement;
  struct leme_render_preview snap_preview;
  struct leme_container_drop container_drop;
};

extern const struct wlr_pointer_grab_interface leme_pointer_grab_interface;

struct leme_pointer_settings
leme_input_pointer_settings(const struct leme_config *config,
                            const char *device_name);
void leme_input_process_motion(struct leme_server *server, uint32_t time_msec,
                               struct leme_render_hit *result,
                               bool allow_view_focus);
void leme_input_pointer_resize_hover(struct leme_server *server,
                                     const struct leme_render_hit *hit);
bool leme_input_pointer_try_compositor_grab(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event, bool inhibited);
bool leme_input_pointer_try_edge_resize(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event);

#endif
