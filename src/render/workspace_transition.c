#include "render/workspace_transition.h"
#include "render/workspace_transition_internal.h"

#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/session.h"
#include "render/animation.h"
#include "render/render.h"
#include "render/workspace_effect.h"
#include "shell/layer.h"
#include "shell/view.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#define LEME_WORKSPACE_SNAPSHOT_CACHE_SIZE 4

struct leme_workspace_snapshot_slot {
  uint16_t tag_id;
  struct wlr_scene_tree *tree;
  const struct leme_view *active_view;
};

struct leme_workspace_transition {
  struct leme_output *output;
  struct wlr_scene_tree *master;
  struct wlr_scene_tree *outgoing;
  struct wlr_scene_tree *incoming;
  struct leme_workspace_snapshot_slot cache[LEME_WORKSPACE_SNAPSHOT_CACHE_SIZE];
  size_t cache_count;
  struct leme_workspace_effect *effect;
  struct leme_workspace_animation_settings settings;
  uint16_t destination_id;
  uint16_t outgoing_id;
  uint16_t incoming_id;
  enum leme_tag_change_direction direction;
  uint16_t ring[LEME_TAGS_RING_MAX];
  size_t ring_count;
  double from_scalar;
  double to_scalar;
  const struct leme_workspace_transition_ops *ops;
  bool is_gesture;
  bool in_animation_manager;
  bool gesture_bounded;
  double gesture_min;
  double gesture_max;
  struct leme_animation_frame last_frame;
  bool last_frame_valid;
  double presented_position;
  bool presented_pair_position;
};

static int leme_workspace_coordinate_subtract(int left, int right) {
  const int64_t value = (int64_t)left - (int64_t)right;

  if (value < INT_MIN) {
    return INT_MIN;
  }
  if (value > INT_MAX) {
    return INT_MAX;
  }
  return (int)value;
}

static bool leme_workspace_view_is_participant(const struct leme_view *view,
                                               const struct leme_output *output,
                                               uint16_t tag_id) {
  return view != NULL && view->render_tree != NULL && view->mapped &&
         !view->unmanaged && !view->detached &&
         leme_ownership_tag(view) != NULL &&
         leme_ownership_tag(view)->owner != NULL &&
         leme_ownership_tag(view)->owner->output == output &&
         leme_ownership_tag(view)->id == tag_id;
}

static const struct leme_view *
leme_workspace_destination_focus(const struct leme_output *output,
                                 uint16_t tag_id) {
  const struct leme_tag *tag;
  const struct leme_view *view;

  if (output == NULL || output->server == NULL ||
      output->server->focused_output != output ||
      leme_layer_keyboard_is_exclusive(output->server) || tag_id == 0 ||
      tag_id > output->tags.max_tags) {
    return NULL;
  }
  tag = output->tags.table[tag_id];
  if (tag == NULL) {
    return NULL;
  }
  if (tag->focused_view != NULL && tag->focused_view->mapped) {
    return tag->focused_view;
  }
  wl_list_for_each(view, &tag->views, tag_link) {
    if (view->mapped) {
      return view;
    }
  }
  return NULL;
}

static bool leme_workspace_tag_has_fullscreen(const struct leme_output *output,
                                              uint16_t tag_id) {
  const struct leme_tag *tag;
  const struct leme_view *view;

  if (tag_id == 0 || tag_id > output->tags.max_tags) {
    return false;
  }
  tag = output->tags.table[tag_id];
  if (tag == NULL) {
    return false;
  }
  wl_list_for_each(view, &tag->views, tag_link) {
    if (view->mapped && view->fullscreen) {
      return true;
    }
  }
  return false;
}

static bool leme_workspace_tag_is_capturable(const struct leme_output *output,
                                             uint16_t tag_id) {
  struct leme_tag *tag;
  struct leme_view *view;

  if (tag_id == 0 || tag_id > output->tags.max_tags) {
    return false;
  }
  tag = output->tags.table[tag_id];
  if (tag == NULL) {
    return true;
  }
  wl_list_for_each(view, &tag->views, tag_link) {
    if (view->mapped && !view->unmanaged && !view->detached &&
        view->render_tree == NULL) {
      return false;
    }
  }
  return true;
}

struct leme_workspace_collection {
  struct wlr_scene_tree *source;
  struct wlr_scene_tree *destination;
};

static bool leme_workspace_collect_parent(
    struct leme_output *output, uint16_t tag_id,
    const struct leme_workspace_collection *collection,
    const struct leme_view *active_view,
    const struct leme_workspace_transition_ops *ops) {
  struct wlr_scene_node *node;

  wl_list_for_each(node, &collection->source->children, link) {
    struct leme_view *view = node->data;
    struct wlr_scene_tree *copy;

    if (!leme_workspace_view_is_participant(view, output, tag_id) ||
        &view->render_tree->node != node) {
      continue;
    }
    copy = ops->snapshot(view->render_tree, collection->destination);
    if (copy == NULL) {
      return false;
    }
    leme_render_view_apply_snapshot(view, copy, view == active_view);
    wlr_scene_node_set_position(
        &copy->node,
        leme_workspace_coordinate_subtract(copy->node.x, output->full_box.x),
        leme_workspace_coordinate_subtract(copy->node.y, output->full_box.y));
  }
  return true;
}

static bool
leme_workspace_collect_tag(struct leme_output *output, uint16_t tag_id,
                           struct wlr_scene_tree *composition,
                           const struct leme_view *active_view,
                           const struct leme_workspace_transition_ops *ops) {
  if (!leme_workspace_tag_is_capturable(output, tag_id)) {
    return false;
  }
  return leme_workspace_collect_parent(
             output, tag_id,
             &(struct leme_workspace_collection){
                 .source = output->server->scene_tiled,
                 .destination = composition,
             },
             active_view, ops) &&
         leme_workspace_collect_parent(
             output, tag_id,
             &(struct leme_workspace_collection){
                 .source = output->server->scene_floating,
                 .destination = composition,
             },
             active_view, ops);
}

static void leme_workspace_transition_discard(
    struct leme_workspace_transition *transition) {
  if (transition == NULL) {
    return;
  }
  leme_workspace_effect_destroy(transition->effect);
  if (transition->master != NULL) {
    wlr_scene_node_destroy(&transition->master->node);
  }
  free(transition);
}

static void
leme_workspace_finish_live_view_animations(struct leme_output *output) {
  struct leme_view *view;

  if (output == NULL || output->server == NULL ||
      output->server->views.next == NULL) {
    return;
  }
  wl_list_for_each(view, &output->server->views, link) {
    if (leme_ownership_tag(view) != NULL &&
        leme_ownership_tag(view)->owner != NULL &&
        leme_ownership_tag(view)->owner->output == output) {
      leme_render_view_finish_animation(view);
    }
  }
}

static bool leme_workspace_transition_is_eligible(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction) {
  struct leme_server *server;
  const struct leme_workspace_animation_settings *settings;

  if (output == NULL || output->server == NULL) {
    return false;
  }
  server = output->server;
  settings =
      server->config == NULL ? NULL : &server->config->workspace_animation;
  return settings != NULL && settings->configured &&
         (settings->kind == LEME_ANIMATION_KIND_SPRING ||
          settings->duration_ms > 0) &&
         source_id != destination_id && output->tags.table != NULL &&
         source_id > 0 && source_id <= output->tags.max_tags &&
         destination_id > 0 && destination_id <= output->tags.max_tags &&
         !leme_workspace_tag_has_fullscreen(output, source_id) &&
         !leme_workspace_tag_has_fullscreen(output, destination_id) &&
         (direction == LEME_TAG_CHANGE_FORWARD ||
          direction == LEME_TAG_CHANGE_BACKWARD) &&
         output->power_on && output->full_box.width > 0 &&
         output->full_box.height > 0 &&
         (output->wlr_output == NULL || output->wlr_output->enabled) &&
         (server->session_protocols == NULL ||
          !server->session_protocols->locked) &&
         (server->session == NULL || server->session->active) &&
         server->scene_floating != NULL && server->scene_tiled != NULL;
}

static void leme_workspace_transition_sync_cache_visibility(
    struct leme_workspace_transition *transition) {
  size_t i;

  if (transition == NULL) {
    return;
  }
  for (i = 0; i < transition->cache_count; i++) {
    const bool one_tag = transition->gesture_bounded &&
                         transition->gesture_min == transition->gesture_max;
    bool active =
        (transition->cache[i].tree == transition->outgoing ||
         (!one_tag && transition->cache[i].tree == transition->incoming));
    if (transition->cache[i].tree != NULL) {
      wlr_scene_node_set_enabled(&transition->cache[i].tree->node, active);
    }
  }
}

static struct leme_workspace_transition *
leme_workspace_transition_prepare_with_ops(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_transition_ops *ops, bool allow_empty) {
  struct leme_workspace_transition *transition = NULL;
  struct leme_workspace_transition *previous =
      output == NULL ? NULL : output->workspace_transition;
  const struct leme_view *destination_focus;
  bool previous_finished = previous == NULL;

  if (!leme_workspace_transition_is_eligible(output, source_id, destination_id,
                                             direction) ||
      ops == NULL || ops->snapshot == NULL || ops->effect_create == NULL) {
    if (output != NULL) {
      if (previous != NULL) {
        leme_render_output_animations_finish(output);
      } else {
        leme_workspace_finish_live_view_animations(output);
        leme_animation_manager_finish_owner(&output->server->animations,
                                            output);
      }
    }
    return NULL;
  }
  destination_focus = leme_workspace_destination_focus(output, destination_id);
  if (previous == NULL) {
    leme_workspace_finish_live_view_animations(output);
    leme_animation_manager_finish_owner(&output->server->animations, output);
  } else {
    leme_workspace_finish_live_view_animations(output);
  }
  transition = calloc(1, sizeof(*transition));
  if (transition == NULL) {
    goto fail;
  }
  transition->output = output;
  transition->settings = output->server->config->workspace_animation;
  transition->destination_id = destination_id;
  transition->outgoing_id = source_id;
  transition->incoming_id = destination_id;
  transition->direction = direction;
  transition->ops = ops;
  transition->ring_count =
      leme_tags_ring(leme_output_tags(output), direction, transition->ring,
                     LEME_ARRAY_LENGTH(transition->ring));
  transition->master = wlr_scene_tree_create(output->server->scene_floating);
  if (transition->master == NULL) {
    goto fail;
  }
  wlr_scene_node_set_enabled(&transition->master->node, false);
  transition->outgoing = wlr_scene_tree_create(transition->master);
  transition->incoming = wlr_scene_tree_create(transition->master);
  if (transition->outgoing == NULL || transition->incoming == NULL) {
    goto fail;
  }
  if (previous == NULL) {
    if (!leme_workspace_collect_tag(output, source_id, transition->outgoing,
                                    NULL, ops)) {
      goto fail;
    }
  } else {
    struct wlr_scene_tree *current =
        ops->snapshot(previous->master, transition->outgoing);

    if (current == NULL) {
      goto fail;
    }
    wlr_scene_node_set_position(
        &current->node,
        leme_workspace_coordinate_subtract(current->node.x, output->full_box.x),
        leme_workspace_coordinate_subtract(current->node.y,
                                           output->full_box.y));
    leme_render_output_animations_finish(output);
    previous_finished = true;
  }
  if (!leme_workspace_collect_tag(output, destination_id, transition->incoming,
                                  destination_focus, ops)) {
    goto fail;
  }
  transition->cache[0] = (struct leme_workspace_snapshot_slot){
      .tag_id = source_id,
      .tree = transition->outgoing,
      .active_view = NULL,
  };
  transition->cache[1] = (struct leme_workspace_snapshot_slot){
      .tag_id = destination_id,
      .tree = transition->incoming,
      .active_view = destination_focus,
  };
  transition->cache_count = 2;
  leme_workspace_transition_sync_cache_visibility(transition);
  transition->effect =
      ops->effect_create(transition->outgoing, transition->incoming,
                         output->full_box, direction, &transition->settings);
  if (transition->effect == NULL ||
      (!allow_empty &&
       !leme_workspace_effect_has_content(transition->effect))) {
    goto fail;
  }
  return transition;

fail:
  if (!previous_finished) {
    leme_render_output_animations_finish(output);
  }
  leme_workspace_transition_discard(transition);
  return NULL;
}

struct leme_workspace_transition *
leme_render_workspace_transition_prepare_with_ops(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_transition_ops *ops) {
  return leme_workspace_transition_prepare_with_ops(
      output, source_id, destination_id, direction, ops, false);
}

static const struct leme_workspace_transition_ops leme_workspace_default_ops = {
    .snapshot = leme_animation_snapshot,
    .effect_create = leme_workspace_effect_create,
};

struct leme_workspace_transition *leme_render_workspace_transition_prepare(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction) {
  return leme_workspace_transition_prepare_with_ops(
      output, source_id, destination_id, direction, &leme_workspace_default_ops,
      false);
}

struct leme_workspace_transition *
leme_render_workspace_transition_prepare_gesture(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction) {
  return leme_workspace_transition_prepare_with_ops(
      output, source_id, destination_id, direction, &leme_workspace_default_ops,
      true);
}

static void
leme_render_workspace_transition_refresh_views(struct leme_output *output) {
  struct leme_view *view;

  if (output == NULL || output->server == NULL ||
      output->server->views.next == NULL) {
    return;
  }
  wl_list_for_each(view, &output->server->views, link) {
    if (leme_ownership_tag(view) != NULL &&
        leme_ownership_tag(view)->owner != NULL &&
        leme_ownership_tag(view)->owner->output == output) {
      leme_render_view_sync_presentation(view);
    }
  }
}

static void leme_render_workspace_transition_apply(
    void *data, const struct leme_animation_frame *frame) {
  struct leme_workspace_transition *transition = data;
  struct leme_output *output;
  double position;

  if (transition == NULL || transition->output == NULL) {
    return;
  }
  output = transition->output;
  position = transition->from_scalar +
             (transition->to_scalar - transition->from_scalar) * frame->scalar;
  leme_tags_position_set(leme_output_tags(output),
                         leme_tags_ring_wrap(position, transition->ring_count));
  leme_workspace_effect_apply(transition->effect, frame);
  transition->last_frame = *frame;
  transition->last_frame_valid = true;
  transition->presented_position = position;
  transition->presented_pair_position = false;
}

static void leme_render_workspace_transition_done(void *data) {
  struct leme_workspace_transition *transition = data;
  struct leme_output *output;

  if (transition == NULL) {
    return;
  }
  output = transition->output;
  if (output != NULL) {
    if (output->workspace_transition == transition) {
      output->workspace_transition = NULL;
      if (leme_output_tags(output) != NULL) {
        leme_output_tags(output)->position_active = false;
      }
    }
    leme_render_workspace_transition_refresh_views(output);
  }
  leme_workspace_effect_destroy(transition->effect);
  transition->effect = NULL;
  if (!transition->in_animation_manager && transition->master != NULL) {
    wlr_scene_node_destroy(&transition->master->node);
    transition->master = NULL;
  }
  free(transition);
}

void leme_render_workspace_transition_commit(
    struct leme_workspace_transition *transition) {
  struct leme_animation_subject subject;
  struct leme_animation_spec spec;
  struct leme_animation_frame initial;
  struct leme_output *output;
  size_t from_index = 0;
  size_t to_index = 0;
  size_t i;

  if (transition == NULL) {
    return;
  }
  output = transition->output;
  output->workspace_transition = transition;
  wlr_scene_node_set_enabled(&transition->master->node, true);
  leme_workspace_transition_sync_cache_visibility(transition);
  leme_render_workspace_transition_refresh_views(output);

  for (i = 0; i < transition->ring_count; i++) {
    if (transition->ring[i] == transition->outgoing_id) {
      from_index = i;
    }
    if (transition->ring[i] == transition->incoming_id) {
      to_index = i;
    }
  }
  transition->from_scalar = (double)from_index;
  transition->to_scalar = (double)to_index;
  if (transition->direction == LEME_TAG_CHANGE_BACKWARD &&
      transition->to_scalar > transition->from_scalar) {
    transition->to_scalar -= (double)transition->ring_count;
  } else if (transition->direction == LEME_TAG_CHANGE_FORWARD &&
             transition->to_scalar < transition->from_scalar) {
    transition->to_scalar += (double)transition->ring_count;
  }

  spec = leme_workspace_effect_animation_spec(transition->effect,
                                              &transition->settings);
  spec.from_scalar = 0.0;
  spec.to_scalar = 1.0;

  initial = leme_animation_frame_at(&spec, 0.0, 0.0);
  leme_workspace_effect_apply(transition->effect, &initial);
  transition->last_frame = initial;
  transition->last_frame_valid = true;
  transition->presented_position = transition->from_scalar;

  leme_tags_position_set(
      leme_output_tags(output),
      leme_tags_ring_wrap(transition->from_scalar, transition->ring_count));

  subject = (struct leme_animation_subject){
      .data = transition,
      .owner = output,
      .apply = leme_render_workspace_transition_apply,
      .done = leme_render_workspace_transition_done,
  };
  transition->in_animation_manager = true;
  leme_animation_run(&output->server->animations, transition->master, &spec,
                     &subject);
  if (output->workspace_transition != NULL && output->wlr_output != NULL) {
    wlr_output_schedule_frame(output->wlr_output);
  }
}

void leme_render_workspace_transition_begin_gesture(
    struct leme_workspace_transition *transition) {
  struct leme_output *output;

  if (transition == NULL) {
    return;
  }
  output = transition->output;
  transition->is_gesture = true;
  transition->in_animation_manager = false;
  output->workspace_transition = transition;
  wlr_scene_node_set_enabled(&transition->master->node, true);
  leme_workspace_transition_sync_cache_visibility(transition);
  if (!transition->last_frame_valid && transition->effect != NULL) {
    struct leme_animation_spec spec = leme_workspace_effect_animation_spec(
        transition->effect, &transition->settings);
    transition->last_frame = leme_animation_frame_at(&spec, 0.0, 0.0);
    leme_workspace_effect_apply(transition->effect, &transition->last_frame);
    transition->last_frame_valid = true;
    for (size_t i = 0; i < transition->ring_count; i++) {
      if (transition->ring[i] == transition->outgoing_id) {
        transition->presented_position = (double)i;
        break;
      }
    }
  }
  leme_render_workspace_transition_refresh_views(output);
}

bool leme_render_workspace_transition_set_gesture_bounds(
    struct leme_output *output, double center_position) {
  if (!isfinite(center_position) || floor(center_position) != center_position) {
    return false;
  }
  return leme_render_workspace_transition_set_gesture_range(
      output, true, center_position - 1.0, center_position + 1.0);
}

static double leme_workspace_gesture_segment(double position, bool bounded,
                                             double minimum, double maximum) {
  if (!bounded) {
    return floor(position);
  }
  return minimum == maximum
             ? minimum
             : fmax(minimum, fmin(floor(position), maximum - 1.0));
}

bool leme_render_workspace_transition_set_gesture_range(
    struct leme_output *output, bool bounded, double minimum, double maximum) {
  struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;
  if (transition == NULL || transition->ring_count < 2 ||
      (bounded && (!isfinite(minimum) || !isfinite(maximum) ||
                   fabs(minimum) > 0x1p52 - 2.0 ||
                   fabs(maximum) > 0x1p52 - 2.0 || minimum > maximum ||
                   floor(minimum) != minimum || floor(maximum) != maximum))) {
    return false;
  }
  const double position = transition->presented_position;
  /* A long keyboard slide blends only its endpoints, not intermediate tags. */
  if (!transition->is_gesture && !transition->presented_pair_position &&
      fabs(transition->to_scalar - transition->from_scalar) > 1.0 &&
      position != transition->from_scalar &&
      position != transition->to_scalar) {
    return false;
  }
  if (transition->last_frame_valid && transition->gesture_bounded &&
      (position != floor(position) || position < transition->gesture_min ||
       position > transition->gesture_max)) {
    const double old_segment = leme_workspace_gesture_segment(
        position, true, transition->gesture_min, transition->gesture_max);
    const double new_segment =
        leme_workspace_gesture_segment(position, bounded, minimum, maximum);
    const bool old_one = transition->gesture_min == transition->gesture_max;
    const bool new_one = bounded && minimum == maximum;
    if (old_segment != new_segment || old_one != new_one) {
      return false;
    }
  }
  transition->gesture_bounded = bounded;
  transition->gesture_min = bounded ? minimum : 0.0;
  transition->gesture_max = bounded ? maximum : 0.0;
  return true;
}

bool leme_render_workspace_transition_presented_position(
    const struct leme_output *output, double *position) {
  const struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;
  if (transition == NULL || position == NULL || !transition->last_frame_valid) {
    return false;
  }
  *position = transition->presented_position;
  return true;
}

void leme_render_workspace_transition_finish(struct leme_output *output) {
  if (output == NULL || output->server == NULL ||
      output->workspace_transition == NULL) {
    return;
  }
  if (output->server->gesture.active && output->server->gesture.engaged &&
      output->server->gesture.output == output) {
    leme_input_workspace_gesture_reset(output->server);
  }
  leme_animation_manager_finish_data(&output->server->animations,
                                     output->workspace_transition);
  if (output->workspace_transition != NULL) {
    leme_render_workspace_transition_done(output->workspace_transition);
  }
}

void leme_render_output_animations_finish(struct leme_output *output) {
  if (output == NULL || output->server == NULL) {
    return;
  }
  struct leme_server *server = output->server;
  leme_workspace_finish_live_view_animations(output);
  leme_animation_manager_finish_owner(&server->animations, output);
  if (server->gesture.active && server->gesture.output == output) {
    if (server->gesture.engaged) {
      const double raw =
          server->gesture.initial_position + server->gesture.displacement;
      double visual =
          server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE
              ? leme_swipe_position(raw, server->gesture.center_position)
              : raw;
      (void)leme_render_workspace_transition_presented_position(output,
                                                                &visual);
      uint16_t tag_id = server->gesture.initial_tag_id;
      if (server->gesture.ring_count > 0 && isfinite(visual)) {
        const size_t wrapped = (size_t)leme_tags_ring_wrap(
            round(visual), server->gesture.ring_count);
        if (wrapped < server->gesture.ring_count) {
          tag_id = server->gesture.ring[wrapped];
        }
      }
      (void)leme_tags_focus_id_direction(leme_output_tags(output), tag_id,
                                         LEME_TAG_CHANGE_FORWARD);
      leme_input_workspace_gesture_reset(server);
      if (output->workspace_transition != NULL) {
        leme_render_workspace_transition_done(output->workspace_transition);
      }
      leme_view_refresh_tag_focus(server);
    } else {
      leme_input_workspace_gesture_reset(server);
      if (output->workspace_transition != NULL) {
        leme_render_workspace_transition_done(output->workspace_transition);
      }
    }
  } else if (output->workspace_transition != NULL) {
    leme_render_workspace_transition_done(output->workspace_transition);
  }
}

bool leme_render_workspace_transition_active(const struct leme_output *output) {
  return output != NULL && output->workspace_transition != NULL;
}

uint16_t
leme_render_workspace_transition_bound_id(const struct leme_output *output,
                                          bool incoming) {
  const struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;

  if (transition == NULL) {
    return 0;
  }
  return incoming ? transition->incoming_id : transition->outgoing_id;
}

const uint16_t *
leme_render_workspace_transition_ring(const struct leme_output *output,
                                      size_t *ring_count) {
  const struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;

  if (transition == NULL || transition->ring_count == 0) {
    if (ring_count != NULL) {
      *ring_count = 0;
    }
    return NULL;
  }
  if (ring_count != NULL) {
    *ring_count = transition->ring_count;
  }
  return transition->ring;
}

bool leme_render_workspace_transition_targets(const struct leme_output *output,
                                              double *from_scalar,
                                              double *to_scalar) {
  const struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;

  if (transition == NULL) {
    return false;
  }
  if (from_scalar != NULL) {
    *from_scalar = transition->from_scalar;
  }
  if (to_scalar != NULL) {
    *to_scalar = transition->to_scalar;
  }
  return true;
}

static struct wlr_scene_tree *leme_workspace_transition_get_or_create_slot(
    struct leme_workspace_transition *transition, uint16_t tag_id,
    const struct leme_view *active_view, uint16_t keep_id) {
  size_t i;
  size_t slot_idx;
  struct wlr_scene_tree *scratch;

  for (i = 0; i < transition->cache_count; i++) {
    if (transition->cache[i].tag_id == tag_id) {
      return transition->cache[i].tree;
    }
  }

  if (transition->cache_count < LEME_WORKSPACE_SNAPSHOT_CACHE_SIZE) {
    slot_idx = transition->cache_count;
  } else {
    slot_idx = LEME_WORKSPACE_SNAPSHOT_CACHE_SIZE;
    for (i = 0; i < transition->cache_count; i++) {
      if (transition->cache[i].tree != transition->outgoing &&
          transition->cache[i].tree != transition->incoming &&
          transition->cache[i].tag_id != keep_id) {
        slot_idx = i;
        break;
      }
    }
    if (slot_idx >= LEME_WORKSPACE_SNAPSHOT_CACHE_SIZE) {
      return NULL;
    }
  }

  scratch = wlr_scene_tree_create(transition->master);
  if (scratch == NULL) {
    return NULL;
  }
  wlr_scene_node_set_enabled(&scratch->node, false);
  if (!leme_workspace_collect_tag(transition->output, tag_id, scratch,
                                  active_view, transition->ops)) {
    wlr_scene_node_destroy(&scratch->node);
    return NULL;
  }

  if (slot_idx < transition->cache_count &&
      transition->cache[slot_idx].tree != NULL) {
    wlr_scene_node_destroy(&transition->cache[slot_idx].tree->node);
  } else if (slot_idx == transition->cache_count) {
    transition->cache_count++;
  }

  transition->cache[slot_idx] = (struct leme_workspace_snapshot_slot){
      .tag_id = tag_id,
      .tree = scratch,
      .active_view = active_view,
  };
  return scratch;
}

static bool leme_workspace_transition_apply_position(struct leme_output *output,
                                                     double position) {
  struct leme_workspace_transition *transition =
      output == NULL ? NULL : output->workspace_transition;
  struct leme_animation_spec spec;
  struct leme_animation_frame frame;
  double segment;
  double fraction;
  size_t low;
  size_t high;
  uint16_t new_outgoing_id;
  uint16_t new_incoming_id;

  if (transition == NULL || transition->ring_count == 0 ||
      !isfinite(position)) {
    return false;
  }

  if (!transition->last_frame_valid && transition->effect != NULL) {
    spec = leme_workspace_effect_animation_spec(transition->effect,
                                                &transition->settings);
    transition->last_frame = leme_animation_frame_at(&spec, 0.0, 0.0);
    transition->last_frame_valid = true;
  }

  segment = leme_workspace_gesture_segment(
      position, transition->gesture_bounded, transition->gesture_min,
      transition->gesture_max);
  fraction = position - segment;
  const double extent =
      fmax((double)output->full_box.width, (double)output->full_box.height);
  if (!isfinite(fraction) ||
      (extent > 0.0 && fabs(fraction) > (double)INT_MAX / extent / 4.0)) {
    return false;
  }
  low = (size_t)leme_tags_ring_wrap(segment, transition->ring_count);
  high = (low + 1) % transition->ring_count;
  new_outgoing_id = transition->ring[low];
  new_incoming_id = transition->ring[high];

  if (transition->outgoing_id != new_outgoing_id ||
      transition->incoming_id != new_incoming_id ||
      transition->direction != LEME_TAG_CHANGE_FORWARD) {
    struct wlr_scene_tree *new_outgoing = transition->outgoing;
    struct wlr_scene_tree *new_incoming = transition->incoming;
    struct leme_workspace_effect *new_effect;
    const struct leme_view *incoming_focus;

    incoming_focus = leme_workspace_destination_focus(output, new_incoming_id);

    if (transition->outgoing_id != new_outgoing_id) {
      new_outgoing = leme_workspace_transition_get_or_create_slot(
          transition, new_outgoing_id, NULL, new_incoming_id);
      if (new_outgoing == NULL) {
        return false;
      }
    }
    if (transition->incoming_id != new_incoming_id) {
      new_incoming = leme_workspace_transition_get_or_create_slot(
          transition, new_incoming_id, incoming_focus, new_outgoing_id);
      if (new_incoming == NULL) {
        return false;
      }
    }

    leme_workspace_effect_restore(transition->effect);
    new_effect = transition->ops->effect_create(
        new_outgoing, new_incoming, output->full_box, LEME_TAG_CHANGE_FORWARD,
        &transition->settings);
    if (new_effect == NULL) {
      if (transition->last_frame_valid) {
        leme_workspace_effect_apply(transition->effect,
                                    &transition->last_frame);
      }
      return false;
    }

    leme_workspace_effect_destroy(transition->effect);
    transition->effect = new_effect;
    transition->direction = LEME_TAG_CHANGE_FORWARD;
    transition->outgoing = new_outgoing;
    transition->outgoing_id = new_outgoing_id;
    transition->incoming = new_incoming;
    transition->incoming_id = new_incoming_id;
    transition->destination_id = new_incoming_id;
    leme_workspace_transition_sync_cache_visibility(transition);
    leme_render_workspace_transition_refresh_views(output);
  }

  spec = leme_workspace_effect_animation_spec(transition->effect,
                                              &transition->settings);
  frame = leme_animation_frame_at(&spec, fraction, fraction);
  if (transition->gesture_bounded &&
      transition->gesture_min == transition->gesture_max) {
    frame.opacity = 1.0;
  }
  leme_workspace_transition_sync_cache_visibility(transition);
  leme_workspace_effect_apply(transition->effect, &frame);
  transition->last_frame = frame;
  transition->last_frame_valid = true;
  transition->presented_position = position;
  transition->presented_pair_position = true;

  leme_tags_position_set(leme_output_tags(output),
                         leme_tags_ring_wrap(position, transition->ring_count));

  if (output->wlr_output != NULL) {
    wlr_output_schedule_frame(output->wlr_output);
  }
  return true;
}

void leme_render_workspace_transition_set_position(struct leme_output *output,
                                                   double position) {
  (void)leme_workspace_transition_apply_position(output, position);
}

static void leme_render_workspace_transition_settle_apply(
    void *data, const struct leme_animation_frame *frame) {
  struct leme_workspace_transition *transition = data;

  if (transition == NULL || transition->output == NULL || frame == NULL) {
    return;
  }
  leme_render_workspace_transition_set_position(transition->output,
                                                frame->scalar);
}

void leme_render_workspace_transition_settle(struct leme_output *output,
                                             double from_position,
                                             double to_position,
                                             double initial_velocity) {
  struct leme_workspace_transition *transition;
  struct leme_animation_spec spec;
  struct leme_animation_subject subject;
  double delta;

  if (output == NULL || output->workspace_transition == NULL) {
    return;
  }
  transition = output->workspace_transition;
  if (!isfinite(from_position) || !isfinite(to_position) ||
      !isfinite(initial_velocity)) {
    leme_render_workspace_transition_finish(output);
    return;
  }
  delta = to_position - from_position;
  if (!isfinite(delta)) {
    leme_render_workspace_transition_finish(output);
    return;
  }
  /* Adopt the release pair before prewarming changes snapshot visibility. */
  if (!leme_workspace_transition_apply_position(output, from_position)) {
    leme_render_workspace_transition_finish(output);
    return;
  }
  if (delta == 0.0 && initial_velocity == 0.0) {
    leme_render_workspace_transition_set_position(output, to_position);
    leme_render_workspace_transition_finish(output);
    return;
  }

  const double num_tags = fabs(round(to_position) - round(from_position));
  if (isfinite(num_tags) && num_tags <= 3.0) {
    const double step = delta >= 0 ? 1.0 : -1.0;
    const double end_idx = round(to_position);
    double cur = floor(from_position);
    size_t count = 0;

    while (count < 4) {
      size_t wrapped =
          (size_t)leme_tags_ring_wrap((double)cur, transition->ring_count);
      uint16_t tag_id = transition->ring[wrapped];
      const struct leme_view *focus =
          leme_workspace_destination_focus(output, tag_id);

      (void)leme_workspace_transition_get_or_create_slot(transition, tag_id,
                                                         focus, 0);
      if (cur == end_idx) {
        break;
      }
      cur += step;
      count++;
    }
    leme_workspace_transition_sync_cache_visibility(transition);
  }

  transition->from_scalar = from_position;
  transition->to_scalar = to_position;

  spec = (struct leme_animation_spec){
      .kind = LEME_ANIMATION_KIND_SPRING,
      .spring = leme_workspace_gesture_spring(&transition->settings),
      .scalar_spring = true,
      .from_scalar = from_position,
      .to_scalar = to_position,
      .scalar_initial_velocity = initial_velocity,
  };

  subject = (struct leme_animation_subject){
      .data = transition,
      .owner = output,
      .apply = leme_render_workspace_transition_settle_apply,
      .done = leme_render_workspace_transition_done,
  };

  transition->in_animation_manager = true;
  leme_animation_run(&output->server->animations, transition->master, &spec,
                     &subject);
  if (output->wlr_output != NULL) {
    wlr_output_schedule_frame(output->wlr_output);
  }
}

bool leme_render_workspace_transition_hides_view(const struct leme_view *view) {
  const struct leme_output *output;
  const struct leme_workspace_transition *transition;
  uint16_t tag_id;

  if (view == NULL || view->unmanaged || view->detached ||
      leme_ownership_tag(view) == NULL ||
      leme_ownership_tag(view)->owner == NULL) {
    return false;
  }
  output = leme_ownership_tag(view)->owner->output;
  transition = output == NULL ? NULL : output->workspace_transition;
  if (transition == NULL) {
    return false;
  }
  tag_id = leme_ownership_tag(view)->id;
  if (transition->is_gesture) {
    return tag_id == transition->outgoing_id ||
           tag_id == transition->incoming_id ||
           tag_id == transition->destination_id ||
           tag_id == output->tags.focused_id;
  }
  return transition->destination_id == tag_id;
}

struct leme_animation_spring leme_workspace_gesture_spring(
    const struct leme_workspace_animation_settings *settings) {
  if (settings != NULL && settings->kind == LEME_ANIMATION_KIND_SPRING) {
    return settings->spring;
  }
  return (struct leme_animation_spring){
      .damping_ratio = 1.0,
      .stiffness = 1000.0,
      .epsilon = 0.0001,
  };
}
