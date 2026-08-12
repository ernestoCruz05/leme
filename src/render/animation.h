#ifndef LEME_ANIMATION_H
#define LEME_ANIMATION_H

#include "core/leme.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>

enum leme_animation_event {
    LEME_ANIMATION_OPEN,
    LEME_ANIMATION_CLOSE,
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

enum leme_animation_effect {
    LEME_ANIMATION_EFFECT_FADE = 1u << 0,
    LEME_ANIMATION_EFFECT_SCALE = 1u << 1,
};

struct leme_animation_settings {
    bool configured;
    uint32_t duration_ms;
    struct leme_animation_curve curve;
    struct leme_animation_curve opacity_curve;
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
};

struct leme_animation_subject {
    void *data;
    void *owner;
    void (*apply)(void *data, const struct leme_animation_frame *frame);
    void (*done)(void *data);
};

struct leme_animation_spec {
    struct leme_box from;
    struct leme_box to;
    double from_opacity;
    double to_opacity;
    uint32_t duration_ms;
    struct leme_animation_curve curve;
    struct leme_animation_curve opacity_curve;
};

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
void leme_animation_manager_finish_all(
    struct leme_animation_manager *manager);
void leme_animation_manager_finish_data(
    struct leme_animation_manager *manager, const void *data);
void leme_animation_manager_finish_owner(
    struct leme_animation_manager *manager, const void *owner);

struct leme_animation_frame leme_animation_frame_at(
    const struct leme_animation_spec *spec,
    double geometry_progress, double opacity_progress);
struct leme_animation_curve leme_animation_curve_preset(
    enum leme_animation_curve_preset preset);
double leme_animation_curve_at(const struct leme_animation_curve *curve,
    double t);
double leme_animation_elapsed(const struct timespec *start,
    const struct timespec *now, uint32_t duration_ms);

#endif
