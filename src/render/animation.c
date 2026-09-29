#include "render/animation.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/transform.h>

struct leme_animation_hold {
  struct wlr_buffer *buffer;
  struct wl_listener destroy;
};

static void leme_animation_hold_handle_destroy(struct wl_listener *listener,
                                               void *data) {
  struct leme_animation_hold *hold = wl_container_of(listener, hold, destroy);

  (void)data;
  wl_list_remove(&hold->destroy.link);
  wlr_buffer_unlock(hold->buffer);
  free(hold);
}

static bool leme_animation_hold_buffer(struct wlr_scene_buffer *node,
                                       struct wlr_buffer *buffer) {
  struct leme_animation_hold *hold = calloc(1, sizeof(*hold));

  if (hold == NULL) {
    return false;
  }
  hold->buffer = wlr_buffer_lock(buffer);
  hold->destroy.notify = leme_animation_hold_handle_destroy;
  wl_signal_add(&node->node.events.destroy, &hold->destroy);
  return true;
}

struct leme_animation_copy_context {
  struct wlr_scene_tree *source;
  struct wlr_scene_tree *target;
};

static bool
leme_animation_copy_children(struct leme_animation_copy_context context) {
  struct wlr_scene_node *node;

  wl_list_for_each(node, &context.source->children, link) {
    struct wlr_scene_node *copy = NULL;

    switch (node->type) {
    case WLR_SCENE_NODE_TREE: {
      struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
      struct wlr_scene_tree *branch = wlr_scene_tree_create(context.target);

      if (branch == NULL ||
          !leme_animation_copy_children((struct leme_animation_copy_context){
              .source = tree,
              .target = branch,
          })) {
        return false;
      }
      copy = &branch->node;
      break;
    }
    case WLR_SCENE_NODE_RECT: {
      struct wlr_scene_rect *rect = wl_container_of(node, rect, node);
      struct wlr_scene_rect *branch = wlr_scene_rect_create(
          context.target, rect->width, rect->height, rect->color);

      if (branch == NULL) {
        return false;
      }
#ifdef LEME_HAVE_EFFECTS
      wlr_scene_rect_set_corner_radius(branch, rect->corner_radius,
                                       rect->border_thickness);
#endif
      copy = &branch->node;
      break;
    }
    case WLR_SCENE_NODE_BUFFER: {
      struct wlr_scene_buffer *buffer = wl_container_of(node, buffer, node);
      struct wlr_scene_buffer *branch;

      if (buffer->buffer == NULL) {
        continue;
      }
      branch = wlr_scene_buffer_create(context.target, buffer->buffer);
      if (branch == NULL ||
          !leme_animation_hold_buffer(branch, buffer->buffer)) {
        return false;
      }
      wlr_scene_buffer_set_source_box(branch, &buffer->src_box);
      wlr_scene_buffer_set_dest_size(branch, buffer->dst_width,
                                     buffer->dst_height);
      wlr_scene_buffer_set_transform(branch, buffer->transform);
      wlr_scene_buffer_set_filter_mode(branch, buffer->filter_mode);
      wlr_scene_buffer_set_opacity(branch, buffer->opacity);
#ifdef LEME_HAVE_EFFECTS
      wlr_scene_buffer_set_corner_radius(branch, buffer->corner_radius);
      wlr_scene_buffer_set_backdrop_blur(branch, buffer->backdrop_blur);
      wlr_scene_buffer_set_backdrop_blur_alpha(branch,
                                               buffer->backdrop_blur_alpha);
#endif
      copy = &branch->node;
      break;
    }
    }
    if (copy == NULL) {
      return false;
    }
    wlr_scene_node_set_position(copy, node->x, node->y);
    wlr_scene_node_set_enabled(copy, node->enabled);
  }
  return true;
}

struct wlr_scene_tree *leme_animation_snapshot(struct wlr_scene_tree *source,
                                               struct wlr_scene_tree *parent) {
  struct wlr_scene_tree *snapshot;

  if (source == NULL || parent == NULL) {
    return NULL;
  }
  snapshot = wlr_scene_tree_create(parent);
  if (snapshot == NULL) {
    return NULL;
  }
  if (!leme_animation_copy_children((struct leme_animation_copy_context){
          .source = source,
          .target = snapshot,
      })) {
    leme_animation_snapshot_destroy(snapshot);
    return NULL;
  }
  wlr_scene_node_set_position(&snapshot->node, source->node.x, source->node.y);
  return snapshot;
}

void leme_animation_snapshot_destroy(struct wlr_scene_tree *snapshot) {
  if (snapshot != NULL) {
    wlr_scene_node_destroy(&snapshot->node);
  }
}

void leme_animation_crop_buffer(
    struct wlr_scene_buffer *buffer,
    const struct leme_animation_buffer_geometry *geometry, int left, int top,
    int width, int height) {
  struct wlr_fbox source = geometry->src_box;
  struct wlr_fbox oriented;
  struct wlr_fbox cropped;
  int transformed_width = geometry->buffer_width;
  int transformed_height = geometry->buffer_height;

  if (geometry->width <= 0 || geometry->height <= 0) {
    return;
  }
  if (wlr_fbox_empty(&source)) {
    source = (struct wlr_fbox){
        .width = geometry->buffer_width,
        .height = geometry->buffer_height,
    };
  }
  wlr_fbox_transform(&oriented, &source, geometry->transform,
                     (double)geometry->buffer_width,
                     (double)geometry->buffer_height);
  cropped = (struct wlr_fbox){
      .x = oriented.x + (double)left * oriented.width / (double)geometry->width,
      .y =
          oriented.y + (double)top * oriented.height / (double)geometry->height,
      .width = (double)width * oriented.width / (double)geometry->width,
      .height = (double)height * oriented.height / (double)geometry->height,
  };
  wlr_output_transform_coords(geometry->transform, &transformed_width,
                              &transformed_height);
  wlr_fbox_transform(&source, &cropped,
                     wlr_output_transform_invert(geometry->transform),
                     (double)transformed_width, (double)transformed_height);
  wlr_scene_buffer_set_source_box(buffer, &source);
  wlr_scene_buffer_set_dest_size(buffer, width, height);
}

struct leme_animation {
  struct wl_list link;
  struct wlr_scene_tree *snapshot;
  struct leme_animation_subject subject;
  struct leme_animation_spec spec;
  struct timespec start;
  struct leme_animation *pending_next;
  bool finishing;
};

static void leme_animation_release(struct leme_animation *animation) {
  leme_animation_snapshot_destroy(animation->snapshot);
  if (animation->subject.done != NULL) {
    animation->subject.done(animation->subject.data);
  }
  free(animation);
}

static int leme_animation_mix(int from, int to, double progress) {
  const double mixed = (double)from + ((double)to - (double)from) * progress;

  if (isnan(mixed)) {
    return to;
  }
  if (mixed <= (double)INT_MIN) {
    return INT_MIN;
  }
  if (mixed >= (double)INT_MAX) {
    return INT_MAX;
  }
  return (int)mixed;
}

static int leme_animation_mix_extent(int from, int to, double progress) {
  const int mixed = leme_animation_mix(from, to, progress);

  return mixed < 0 ? 0 : mixed;
}

struct leme_animation_frame
leme_animation_frame_at(const struct leme_animation_spec *spec,
                        double geometry_progress, double opacity_progress) {
  return (struct leme_animation_frame){
      .box =
          {
              .x = leme_animation_mix(spec->from.x, spec->to.x,
                                      geometry_progress),
              .y = leme_animation_mix(spec->from.y, spec->to.y,
                                      geometry_progress),
              .width = leme_animation_mix_extent(
                  spec->from.width, spec->to.width, geometry_progress),
              .height = leme_animation_mix_extent(
                  spec->from.height, spec->to.height, geometry_progress),
          },
      .opacity = spec->from_opacity +
                 (spec->to_opacity - spec->from_opacity) * opacity_progress,
      .scalar = spec->from_scalar +
                (spec->to_scalar - spec->from_scalar) * geometry_progress,
  };
}

static double
leme_animation_spring_beta(const struct leme_animation_spring *spring) {
  return spring->damping_ratio * sqrt(spring->stiffness);
}

static uint32_t leme_animation_seconds_to_ms(double seconds) {
  const double ms = ceil(seconds * 1000.0);

  if (!isfinite(ms) || ms <= 0.0) {
    return 0;
  }
  if (ms >= (double)LEME_ANIMATION_SPRING_SETTLE_MAX_MS) {
    return LEME_ANIMATION_SPRING_SETTLE_MAX_MS;
  }
  return (uint32_t)ms;
}

double leme_animation_spring_displacement_at(
    const struct leme_animation_spring *spring, double displacement,
    double velocity, double seconds) {
  if (spring == NULL || !isfinite(displacement) || !isfinite(velocity) ||
      !isfinite(seconds) || !(spring->stiffness > 0.0) ||
      !(spring->damping_ratio >= 0.0) || !(spring->epsilon > 0.0)) {
    return isfinite(displacement) ? displacement : 0.0;
  }
  if (seconds <= 0.0) {
    return displacement;
  }
  if (displacement == 0.0 && velocity == 0.0) {
    return 0.0;
  }

  const double beta = leme_animation_spring_beta(spring);
  const double omega0 = sqrt(spring->stiffness);
  const double slope = beta * displacement + velocity;

  if (fabs(beta - omega0) <= (double)FLT_EPSILON) {
    return exp(-beta * seconds) * (displacement + slope * seconds);
  }
  if (beta < omega0) {
    const double omega1 = sqrt(omega0 * omega0 - beta * beta);
    return exp(-beta * seconds) * (displacement * cos(omega1 * seconds) +
                                   (slope / omega1) * sin(omega1 * seconds));
  }
  {
    const double omega2 = sqrt(beta * beta - omega0 * omega0);
    const double coefficient = slope / omega2;
    return (displacement + coefficient) / 2.0 * exp((omega2 - beta) * seconds) +
           (displacement - coefficient) / 2.0 * exp(-(omega2 + beta) * seconds);
  }
}

uint32_t leme_animation_spring_displacement_duration_ms(
    const struct leme_animation_spring *spring, double displacement,
    double velocity) {
  double beta;
  double omega0;
  double slope;
  double alpha;
  double t_hi;
  double t_ext = -1.0;
  double lo;
  double hi;
  int iteration;

  if (spring == NULL || !isfinite(displacement) || !isfinite(velocity) ||
      !(spring->stiffness > 0.0) || !(spring->damping_ratio >= 0.0) ||
      !(spring->epsilon > 0.0)) {
    return 0;
  }
  if (displacement == 0.0 && velocity == 0.0) {
    return 0;
  }
  beta = leme_animation_spring_beta(spring);
  omega0 = sqrt(spring->stiffness);
  if (!(beta > (double)FLT_EPSILON)) {
    return 0;
  }
  slope = beta * displacement + velocity;
  if (beta < omega0) {
    const double omega1 = sqrt(omega0 * omega0 - beta * beta);
    const double a = hypot(displacement, slope / omega1);
    if (a <= spring->epsilon) {
      return 0;
    }
    const double envelope_epsilon = spring->epsilon / a;
    double t_settle = -log(envelope_epsilon) / beta;
    return leme_animation_seconds_to_ms(t_settle);
  }

  alpha = fabs(beta - omega0) <= (double)FLT_EPSILON
              ? beta
              : beta - sqrt(beta * beta - omega0 * omega0);
  if (alpha <= (double)FLT_EPSILON) {
    alpha = beta * 0.01;
  }

  if (fabs(beta - omega0) <= (double)FLT_EPSILON) {
    if (fabs(beta * slope) > 1e-9) {
      const double candidate = velocity / (beta * slope);
      if (candidate > 0.0) {
        t_ext = candidate;
      }
    }
  } else {
    const double omega2 = sqrt(beta * beta - omega0 * omega0);
    const double coeff = slope / omega2;
    const double c1 = (displacement + coeff) / 2.0;
    const double c2 = (displacement - coeff) / 2.0;
    const double r1 = -beta + omega2;
    const double r2 = -beta - omega2;

    if (fabs(c1 * r1) > 1e-9) {
      const double ratio = -(c2 * r2) / (c1 * r1);
      if (ratio > 0.0) {
        const double candidate = log(ratio) / (r1 - r2);
        if (candidate > 0.0) {
          t_ext = candidate;
        }
      }
    }
  }

  double scale = fabs(displacement);
  if (scale < spring->epsilon) {
    scale = spring->epsilon;
  }
  if (t_ext > 0.0) {
    const double x_ext = fabs(leme_animation_spring_displacement_at(
        spring, displacement, velocity, t_ext));
    if (x_ext > scale) {
      scale = x_ext;
    }
  }

  if (scale <= spring->epsilon && t_ext <= 0.0) {
    return 0;
  }

  t_hi = -log(spring->epsilon / scale) / alpha;
  if (t_hi < 0.1) {
    t_hi = 0.1;
  }

  while ((t_hi < t_ext ||
          fabs(leme_animation_spring_displacement_at(
              spring, displacement, velocity, t_hi)) > spring->epsilon) &&
         t_hi < 10.0) {
    t_hi *= 1.5;
  }

  if (t_ext > 0.0) {
    if (fabs(leme_animation_spring_displacement_at(
            spring, displacement, velocity, t_ext)) > spring->epsilon) {
      lo = t_ext;
      hi = t_hi;
    } else if (fabs(displacement) <= spring->epsilon) {
      return 0;
    } else {
      lo = 0.0;
      hi = t_ext;
    }
  } else {
    if (fabs(displacement) <= spring->epsilon) {
      return 0;
    }
    lo = 0.0;
    hi = t_hi;
  }

  for (iteration = 0; iteration < 32; iteration++) {
    const double mid = (lo + hi) / 2.0;
    const double y = leme_animation_spring_displacement_at(spring, displacement,
                                                           velocity, mid);

    if (fabs(y) > spring->epsilon) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return leme_animation_seconds_to_ms(hi);
}

double
leme_animation_spring_value_at(const struct leme_animation_spring *spring,
                               double initial_velocity, double seconds) {
  if (spring == NULL || seconds <= 0.0) {
    return 0.0;
  }
  return 1.0 + leme_animation_spring_displacement_at(spring, -1.0,
                                                     initial_velocity, seconds);
}

uint32_t
leme_animation_spring_duration_ms(const struct leme_animation_spring *spring,
                                  double initial_velocity) {
  if (spring == NULL) {
    return 0;
  }
  return leme_animation_spring_displacement_duration_ms(spring, -1.0,
                                                        initial_velocity);
}

static bool
leme_animation_manager_ready(const struct leme_animation_manager *manager) {
  return manager != NULL && manager->animations.next != NULL;
}

void leme_animation_manager_init(struct leme_animation_manager *manager) {
  wl_list_init(&manager->animations);
  manager->dispatching = false;
}

void leme_animation_manager_finish(struct leme_animation_manager *manager) {
  leme_animation_manager_finish_all(manager);
}

bool leme_animation_manager_active(
    const struct leme_animation_manager *manager) {
  return leme_animation_manager_ready(manager) &&
         !wl_list_empty(&manager->animations);
}

static struct leme_animation *
leme_animation_manager_take_finishing(struct leme_animation_manager *manager) {
  struct leme_animation *animation;
  struct leme_animation *next;
  struct leme_animation *pending = NULL;
  struct leme_animation **pending_tail = &pending;

  wl_list_for_each_safe(animation, next, &manager->animations,
                        link) { // NOLINT(clang-analyzer-unix.Malloc)
    if (animation->finishing) {
      wl_list_remove(&animation->link);
      animation->pending_next = NULL;
      *pending_tail = animation;
      pending_tail = &animation->pending_next;
    }
  }
  return pending;
}

static void
leme_animation_manager_drain(struct leme_animation_manager *manager) {
  struct leme_animation *pending;

  if (!leme_animation_manager_ready(manager) || manager->dispatching) {
    return;
  }
  manager->dispatching = true;
  while ((pending = leme_animation_manager_take_finishing(manager)) != NULL) {
    while (pending != NULL) {
      struct leme_animation *animation = pending;

      pending = animation->pending_next;
      animation->pending_next = NULL;
      leme_animation_release(animation);
    }
  }
  manager->dispatching = false;
}

void leme_animation_manager_finish_all(struct leme_animation_manager *manager) {
  struct leme_animation *animation;

  if (!leme_animation_manager_ready(manager)) {
    return;
  }
  wl_list_for_each(animation, &manager->animations, link) {
    animation->finishing = true;
  }
  leme_animation_manager_drain(manager);
}

void leme_animation_manager_finish_data(struct leme_animation_manager *manager,
                                        const void *data) {
  struct leme_animation *animation;

  if (!leme_animation_manager_ready(manager) || data == NULL) {
    return;
  }
  wl_list_for_each(animation, &manager->animations, link) {
    if (animation->subject.data == data) {
      animation->finishing = true;
    }
  }
  leme_animation_manager_drain(manager);
}

void leme_animation_manager_cancel_data(struct leme_animation_manager *manager,
                                        const void *data) {
  struct leme_animation *animation;
  struct leme_animation *temporary;

  if (!leme_animation_manager_ready(manager) || data == NULL) {
    return;
  }
  wl_list_for_each_safe(animation, temporary, &manager->animations, link) {
    if (animation->subject.data == data) {
      wl_list_remove(&animation->link);
      free(animation);
    }
  }
}

void leme_animation_manager_finish_owner(struct leme_animation_manager *manager,
                                         const void *owner) {
  struct leme_animation *animation;

  if (!leme_animation_manager_ready(manager) || owner == NULL) {
    return;
  }
  wl_list_for_each(animation, &manager->animations, link) {
    if (animation->subject.owner == owner) {
      animation->finishing = true;
    }
  }
  leme_animation_manager_drain(manager);
}

bool leme_animation_manager_active_for_owner(
    struct leme_animation_manager *manager, const void *owner) {
  struct leme_animation *animation;

  if (!leme_animation_manager_ready(manager) || owner == NULL) {
    return false;
  }
  wl_list_for_each(animation, &manager->animations, link) {
    if (animation->subject.owner == owner || animation->subject.owner == NULL) {
      return true;
    }
  }
  return false;
}

void leme_animation_run(struct leme_animation_manager *manager,
                        struct wlr_scene_tree *snapshot,
                        const struct leme_animation_spec *spec,
                        const struct leme_animation_subject *subject) {
  struct leme_animation *animation;
  uint32_t duration;

  if (snapshot == NULL || spec == NULL || subject == NULL) {
    goto abandon;
  }
  if (spec->kind == LEME_ANIMATION_KIND_SPRING) {
    duration = spec->scalar_spring
                   ? leme_animation_spring_displacement_duration_ms(
                         &spec->spring, spec->from_scalar - spec->to_scalar,
                         spec->scalar_initial_velocity)
                   : leme_animation_spring_duration_ms(&spec->spring,
                                                       spec->initial_velocity);
  } else {
    duration = spec->duration_ms;
  }
  if (!leme_animation_manager_ready(manager) || duration == 0) {
    goto abandon;
  }
  animation = calloc(1, sizeof(*animation));
  if (animation == NULL) {
    goto abandon;
  }
  animation->snapshot = snapshot;
  animation->spec = *spec;
  animation->spec.duration_ms = duration;
  animation->subject = *subject;
  clock_gettime(CLOCK_MONOTONIC, &animation->start);
  wl_list_insert(&manager->animations, &animation->link);
  return;

abandon:
  leme_animation_snapshot_destroy(snapshot);
  if (subject != NULL && subject->done != NULL) {
    subject->done(subject->data);
  }
}

static bool leme_animation_holds(const struct leme_animation *animation,
                                 const struct timespec *now) {
  const int64_t elapsed_ms =
      ((int64_t)now->tv_sec - (int64_t)animation->start.tv_sec) * 1000 +
      ((int64_t)now->tv_nsec - (int64_t)animation->start.tv_nsec) / 1000000;

  return animation->spec.hold_max_ms > 0 && animation->subject.ready != NULL &&
         elapsed_ms < (int64_t)animation->spec.duration_ms +
                          (int64_t)animation->spec.hold_max_ms &&
         !animation->subject.ready(animation->subject.data);
}

void leme_animation_manager_tick(struct leme_animation_manager *manager,
                                 const struct timespec *now) {
  struct leme_animation *animation;
  struct leme_animation *next;

  if (!leme_animation_manager_ready(manager) || now == NULL ||
      manager->dispatching) {
    return;
  }
  manager->dispatching = true;
  wl_list_for_each_safe(animation, next, &manager->animations, link) {
    double linear;
    struct leme_animation_frame frame;
    double geometry;
    double opacity;

    if (animation->finishing) {
      continue;
    }
    linear = leme_animation_elapsed(&animation->start, now,
                                    animation->spec.duration_ms);
    if (linear >= 1.0) {
      if (leme_animation_holds(animation, now)) {
        frame = leme_animation_frame_at(&animation->spec, 1.0, 1.0);
        if (animation->subject.apply != NULL) {
          animation->subject.apply(animation->subject.data, &frame);
        }
        continue;
      }
      animation->finishing = true;
      continue;
    }
    if (animation->spec.kind == LEME_ANIMATION_KIND_SPRING &&
        animation->spec.scalar_spring) {
      const double elapsed_seconds =
          (double)animation->spec.duration_ms * linear / 1000.0;
      frame = leme_animation_frame_at(&animation->spec, 0.0, 0.0);
      frame.scalar =
          animation->spec.to_scalar +
          leme_animation_spring_displacement_at(
              &animation->spec.spring,
              animation->spec.from_scalar - animation->spec.to_scalar,
              animation->spec.scalar_initial_velocity, elapsed_seconds);
      if (!isfinite(frame.scalar)) {
        animation->finishing = true;
        continue;
      }
    } else if (animation->spec.kind == LEME_ANIMATION_KIND_SPRING) {
      geometry = leme_animation_spring_value_at(
          &animation->spec.spring, animation->spec.initial_velocity,
          linear * (double)animation->spec.duration_ms / 1000.0);
      if (isnan(geometry)) {
        geometry = 1.0;
        opacity = 1.0;
      } else {
        opacity = geometry < 0.0 ? 0.0 : (geometry > 1.0 ? 1.0 : geometry);
      }
      frame = leme_animation_frame_at(&animation->spec, geometry, opacity);
    } else {
      geometry = leme_animation_curve_at(&animation->spec.curve, linear);
      opacity = leme_animation_curve_at(&animation->spec.opacity_curve, linear);
      frame = leme_animation_frame_at(&animation->spec, geometry, opacity);
    }
    if (animation->subject.apply != NULL) {
      animation->subject.apply(animation->subject.data, &frame);
    }
  }
  manager->dispatching = false;
  leme_animation_manager_drain(manager);
}

#define LEME_ANIMATION_BAKED_POINTS 64

struct leme_animation_curve
leme_animation_curve_preset(enum leme_animation_curve_preset preset) {
  switch (preset) {
  case LEME_ANIMATION_CURVE_EASE_IN:
    return (struct leme_animation_curve){0.42, 0.0, 1.0, 1.0};
  case LEME_ANIMATION_CURVE_EASE_OUT:
    return (struct leme_animation_curve){0.0, 0.0, 0.58, 1.0};
  case LEME_ANIMATION_CURVE_EASE_IN_OUT:
    return (struct leme_animation_curve){0.42, 0.0, 0.58, 1.0};
  case LEME_ANIMATION_CURVE_LINEAR:
    break;
  }
  return (struct leme_animation_curve){0.0, 0.0, 1.0, 1.0};
}

static double leme_animation_bezier(double a, double b, double t) {
  double inverse = 1.0 - t;

  return 3.0 * t * inverse * inverse * a + 3.0 * t * t * inverse * b +
         t * t * t;
}

double leme_animation_curve_at(const struct leme_animation_curve *curve,
                               double t) {
  double low = 0.0;
  double high = 1.0;
  int step;

  if (t <= 0.0) {
    return 0.0;
  }
  if (t >= 1.0) {
    return 1.0;
  }
  for (step = 0; step < LEME_ANIMATION_BAKED_POINTS; step++) {
    double middle = (low + high) / 2.0;
    double x = leme_animation_bezier(curve->x1, curve->x2, middle);

    if (x < t) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return leme_animation_bezier(curve->y1, curve->y2, (low + high) / 2.0);
}

double leme_animation_elapsed(const struct timespec *start,
                              const struct timespec *now,
                              uint32_t duration_ms) {
  double seconds;
  double progress;

  if (duration_ms == 0) {
    return 1.0;
  }
  seconds = (double)(now->tv_sec - start->tv_sec) +
            ((double)now->tv_nsec - (double)start->tv_nsec) / 1000000000.0;
  progress = seconds * 1000.0 / (double)duration_ms;
  if (progress <= 0.0) {
    return 0.0;
  }
  if (progress >= 1.0) {
    return 1.0;
  }
  return progress;
}
