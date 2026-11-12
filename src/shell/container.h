#ifndef LEME_SHELL_CONTAINER_H
#define LEME_SHELL_CONTAINER_H

#include "public/identity.h"
#include "desktop/frame.h"
#include "render/owner.h"
#include "shell/placement.h"
#include "workspace/layout.h"

#include <wayland-server-core.h>

struct leme_decoration_window;
struct leme_server;
struct leme_tag;
struct leme_view;
struct leme_animation_gate;
struct leme_render_container_animation;

struct leme_container_drop {
  struct leme_public_id source;
  struct leme_public_id target;
  enum leme_direction direction;
  struct leme_box preview;
  double x;
  double y;
};

struct leme_container {
  struct leme_tag *tag;
  struct leme_decoration_window *decoration_window;
  struct wlr_scene_tree *render_tree;
  struct wlr_scene_rect *border[4];
  struct leme_desk_frame frame;
  struct leme_render_container_animation *animation_state;
  struct leme_animation_gate *animation_gate;
  struct leme_box render_box;
  struct leme_box arranged_content;
  struct leme_render_owner render_owner;
  struct wl_list link;
  struct leme_view **views;
  size_t count;
  struct leme_view *focused;
  struct leme_layout layout;
  struct leme_placement_state placement;
  struct leme_box box;
  struct leme_box output_box;
  struct leme_public_id id;
  uint64_t generation;
  bool minimized;
  bool render_visible;
  uint64_t minimized_order;
};

bool leme_container_minimize_available(const struct leme_container *container);
bool leme_container_set_minimized(struct leme_container *container,
                                  bool minimized);
bool leme_container_set_layout(struct leme_container *container,
                               enum leme_layout_kind kind);
bool leme_container_swap_direction(struct leme_view *view,
                                   enum leme_direction direction);
bool leme_container_join(struct leme_view *view, enum leme_direction direction);
bool leme_container_can_place(const struct leme_container *container,
                              enum leme_view_placement kind);
bool leme_container_set_placement(struct leme_container *container,
                                  enum leme_view_placement kind);
void leme_container_restore_placement(struct leme_container *container);
bool leme_container_unplace_for_resize(struct leme_container *container);
bool leme_container_apply_interactive_box(struct leme_container *container,
                                          struct leme_box box);
bool leme_container_member_eligible(const struct leme_view *view);
bool leme_container_action_allowed(const struct leme_view *view);
bool leme_container_detach_current(struct leme_view *view);
void leme_container_reconcile_view(struct leme_view *view);
struct leme_container *leme_container_for_view(const struct leme_view *view);
void leme_container_focus_view(struct leme_view *view);
bool leme_container_prepare_drop(struct leme_view *target,
                                 struct leme_view *added, double lx, double ly,
                                 struct leme_container_drop *out);
bool leme_container_commit_drop(struct leme_server *server,
                                const struct leme_container_drop *drop);
bool leme_container_dissolve(struct leme_container *container);
bool leme_container_close(struct leme_container *container);
void leme_container_remove_view(struct leme_view *view);
void leme_container_arrange(struct leme_container *container, bool animate);
void leme_container_reconcile_geometry(struct leme_server *server);
bool leme_container_move_to_output(struct leme_container *container,
                                   struct leme_output *output, bool follow);
bool leme_container_adopt_output(struct leme_container *container,
                                 struct leme_output *output, uint16_t tag_id);
void leme_container_tag_finish(struct leme_tag *tag);
struct leme_box
leme_container_content_box(const struct leme_container *container,
                           struct leme_box frame);
struct leme_container *leme_container_by_public_id(struct leme_server *server,
                                                   struct leme_public_id id);

#endif
