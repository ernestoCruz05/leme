#ifndef LEME_ANIMATION_H
#define LEME_ANIMATION_H

#include "core/leme.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

enum leme_animation_event {
  LEME_ANIMATION_OPEN,
  LEME_ANIMATION_CLOSE,
  LEME_ANIMATION_MOVE,
  LEME_ANIMATION_EVENT_COUNT,
};

enum leme_animation_curve_preset {
  LEME_ANIMATION_CURVE_LINEAR,
  LEME_ANIMATION_CURVE_EASE_IN,
  LEME_ANIMATION_CURVE_EASE_OUT,
  LEME_ANIMATION_CURVE_EASE_IN_OUT,
};

struct leme_animation_curve {
  double x1;
  double y1;
  double x2;
  double y2;
};

struct leme_animation_spring {
  double damping_ratio;
  double stiffness;
  double epsilon;
};

enum leme_animation_kind {
  LEME_ANIMATION_KIND_EASING,
  LEME_ANIMATION_KIND_SPRING,
};

enum leme_animation_effect {
  LEME_ANIMATION_EFFECT_FADE = 1u << 0,
  LEME_ANIMATION_EFFECT_SCALE = 1u << 1,
};

struct leme_animation_settings {
  bool configured;
  uint32_t duration_ms;
  struct leme_animation_curve curve;
  struct leme_animation_curve opacity_curve;
  enum leme_animation_kind kind;
  struct leme_animation_spring spring;
  uint32_t effects;
  double scale_from;
};

struct wlr_scene_tree;

struct leme_animation_manager {
  struct wl_list animations;
  bool dispatching;
};

struct leme_animation_frame {
  struct leme_box box;
  double opacity;
  double scalar;
};

struct leme_animation_subject {
  void *data;
  const void *owner;
  void (*apply)(void *data, const struct leme_animation_frame *frame);
  void (*done)(void *data);
  bool (*ready)(void *data);
};

struct leme_animation_spec {
  struct leme_box from;
  struct leme_box to;
  double from_opacity;
  double to_opacity;
  double from_scalar;
  double to_scalar;
  uint32_t duration_ms;
  struct leme_animation_curve curve;
  struct leme_animation_curve opacity_curve;
  enum leme_animation_kind kind;
  double initial_velocity;
  struct leme_animation_spring spring;
  bool scalar_spring;
  double scalar_initial_velocity;
  uint32_t hold_max_ms;
};

struct leme_animation_buffer_geometry {
  int width;
  int height;
  int buffer_width;
  int buffer_height;
  struct wlr_fbox src_box;
  enum wl_output_transform transform;
};

struct wlr_scene_buffer;
void leme_animation_crop_buffer(
    struct wlr_scene_buffer *buffer,
    const struct leme_animation_buffer_geometry *geometry, int left, int top,
    int width, int height);
struct wlr_scene_tree *leme_animation_snapshot(struct wlr_scene_tree *source,
                                               struct wlr_scene_tree *parent);
void leme_animation_snapshot_destroy(struct wlr_scene_tree *snapshot);

void leme_animation_manager_init(struct leme_animation_manager *manager);
void leme_animation_manager_finish(struct leme_animation_manager *manager);
void leme_animation_run(struct leme_animation_manager *manager,
                        struct wlr_scene_tree *snapshot,
                        const struct leme_animation_spec *spec,
                        const struct leme_animation_subject *subject);
void leme_animation_manager_tick(struct leme_animation_manager *manager,
                                 const struct timespec *now);
bool leme_animation_manager_active(
    const struct leme_animation_manager *manager);
bool leme_animation_manager_active_for_owner(
    struct leme_animation_manager *manager, const void *owner);
void leme_animation_manager_finish_all(struct leme_animation_manager *manager);
void leme_animation_manager_finish_data(struct leme_animation_manager *manager,
                                        const void *data);
void leme_animation_manager_cancel_data(struct leme_animation_manager *manager,
                                        const void *data);
void leme_animation_manager_finish_owner(struct leme_animation_manager *manager,
                                         const void *owner);

struct leme_animation_frame
leme_animation_frame_at(const struct leme_animation_spec *spec,
                        double geometry_progress, double opacity_progress);
struct leme_animation_curve
leme_animation_curve_preset(enum leme_animation_curve_preset preset);
double leme_animation_curve_at(const struct leme_animation_curve *curve,
                               double t);
double leme_animation_elapsed(const struct timespec *start,
                              const struct timespec *now, uint32_t duration_ms);
#define LEME_ANIMATION_SPRING_SETTLE_MAX_MS 10000u

double
leme_animation_spring_value_at(const struct leme_animation_spring *spring,
                               double initial_velocity, double seconds);
uint32_t
leme_animation_spring_duration_ms(const struct leme_animation_spring *spring,
                                  double initial_velocity);

double leme_animation_spring_displacement_at(
    const struct leme_animation_spring *spring, double displacement,
    double velocity, double seconds);
uint32_t leme_animation_spring_displacement_duration_ms(
    const struct leme_animation_spring *spring, double displacement,
    double velocity);

#endif
