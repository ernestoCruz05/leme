#include "input/pointer_internal.h"
#include "input/input.h"
#include "input/internal.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/session.h"
#include "render/animation.h"
#include "render/workspace_transition.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <libinput.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>

static bool leme_input_gesture_natural_scroll(struct leme_server *server,
                                              struct wlr_input_device *device) {
  if (device != NULL && wlr_input_device_is_libinput(device)) {
    struct libinput_device *libinput_device =
        wlr_libinput_get_device_handle(device);

    if (libinput_device != NULL &&
        libinput_device_config_scroll_has_natural_scroll(libinput_device)) {
      return libinput_device_config_scroll_get_natural_scroll_enabled(
                 libinput_device) != 0;
    }
  }
  return leme_input_pointer_settings(server->config,
                                     device != NULL ? device->name : NULL)
      .natural_scroll;
}

void leme_input_workspace_gesture_reset(struct leme_server *server) {
  if (server == NULL) {
    return;
  }
  if (!leme_gesture_accel_restore(&server->gesture.accel)) {
    wlr_log(WLR_ERROR, "%s",
            "leme: failed to restore gesture pointer acceleration");
  }
  server->gesture = (struct leme_workspace_gesture_state){0};
}

void leme_input_workspace_gesture_cancel(struct leme_server *server) {
  struct leme_output *output;
  bool was_engaged;

  if (server == NULL) {
    return;
  }
  const bool was_active = server->gesture.active;
  output = server->gesture.output;
  was_engaged = server->gesture.engaged;

  leme_input_workspace_gesture_reset(server);

  if (output != NULL && was_engaged && output->workspace_transition != NULL) {
    leme_render_workspace_transition_finish(output);
  }
  if (was_active && server->started && server->scene != NULL) {
    leme_view_refresh_tag_focus(server);
  }
}

static size_t leme_input_gesture_ring(const struct leme_tags *tags,
                                      bool occupied, uint16_t *ring,
                                      size_t capacity) {
  return occupied
             ? leme_tags_occupied_ring(tags, ring, capacity)
             : leme_tags_ring(tags, LEME_TAG_CHANGE_FORWARD, ring, capacity);
}

enum leme_workspace_swipe leme_input_workspace_swipe_begin(
    struct leme_server *server,
    const struct wlr_pointer_swipe_begin_event *event) {
  struct leme_output *output;
  struct wlr_input_device *device;
  struct libinput_device *libinput_device = NULL;
  bool natural_scroll;
  uint16_t focused_id;
  double initial_pos;
  uint16_t ring[LEME_TAGS_RING_MAX];
  size_t ring_count = 0;
  size_t center_index;
  bool is_takeover;

  if (server == NULL || server->config == NULL || event == NULL) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }
  if (server->config->gestures.workspace_switch.fingers == 0 ||
      event->fingers != server->config->gestures.workspace_switch.fingers) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  const enum leme_workspace_gesture_mode mode =
      server->config->gestures.workspace_switch.mode;
  const bool occupied = server->config->gestures.workspace_switch.occupied;
  leme_input_workspace_gesture_cancel(server);

  if (leme_input_pointer_grab_active(server) || leme_session_locked(server)) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }
  output = leme_output_focused(server);
  if (output == NULL || leme_output_tags(output) == NULL ||
      output->wlr_output == NULL || !output->wlr_output->enabled) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  device = event->pointer != NULL ? &event->pointer->base : NULL;
  natural_scroll = leme_input_gesture_natural_scroll(server, device);
  focused_id = leme_output_tags(output)->focused_id;
  initial_pos = leme_tags_position(leme_output_tags(output));

  is_takeover = (output->workspace_transition != NULL &&
                 leme_render_workspace_transition_active(output));

  if (is_takeover && !leme_render_workspace_transition_presented_position(
                         output, &initial_pos)) {
    leme_render_workspace_transition_finish(output);
    is_takeover = false;
    initial_pos = leme_tags_position(leme_output_tags(output));
  }
  if (is_takeover) {
    size_t tr_count = 0;
    const uint16_t *tr_ring =
        leme_render_workspace_transition_ring(output, &tr_count);
    if (tr_ring != NULL && tr_count >= 2 && tr_count <= LEME_TAGS_RING_MAX) {
      memcpy(ring, tr_ring, tr_count * sizeof(uint16_t));
      ring_count = tr_count;
    }
  }
  if (ring_count == 0) {
    ring_count = leme_input_gesture_ring(leme_output_tags(output), occupied,
                                         ring, LEME_ARRAY_LENGTH(ring));
  }
  if (ring_count < 2 || ring_count > LEME_TAGS_RING_MAX) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  center_index = ring_count;
  for (size_t i = 0; i < ring_count; i++) {
    if (ring[i] == focused_id) {
      center_index = i;
      break;
    }
  }
  if (center_index >= ring_count) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }
  if (occupied && !is_takeover) {
    initial_pos = (double)center_index;
  }

  const double count = (double)ring_count;
  const double index = (double)center_index;
  double center_pos = index + count * round((initial_pos - index) / count);
  bool ring_current = true;

  if (is_takeover && occupied) {
    uint16_t current[LEME_TAGS_RING_MAX];

    ring_current =
        leme_tags_occupied_ring(leme_output_tags(output), current,
                                LEME_ARRAY_LENGTH(current)) == ring_count &&
        memcmp(current, ring, ring_count * sizeof(*ring)) == 0;
  }
  if (is_takeover && ((mode == LEME_WORKSPACE_GESTURE_SINGLE &&
                       fabs(initial_pos - center_pos) > 1.0) ||
                      !ring_current ||
                      !leme_render_workspace_transition_set_gesture_range(
                          output, mode == LEME_WORKSPACE_GESTURE_SINGLE,
                          center_pos - 1.0, center_pos + 1.0))) {
    leme_render_workspace_transition_finish(output);
    is_takeover = false;
    focused_id = leme_output_tags(output)->focused_id;
    initial_pos = leme_tags_position(leme_output_tags(output));
    ring_count = leme_input_gesture_ring(leme_output_tags(output), occupied,
                                         ring, LEME_ARRAY_LENGTH(ring));
    if (ring_count < 2 || ring_count > LEME_TAGS_RING_MAX) {
      return LEME_WORKSPACE_SWIPE_IGNORED;
    }
    center_index = ring_count;
    for (size_t i = 0; i < ring_count; i++) {
      if (ring[i] == focused_id) {
        center_index = i;
        break;
      }
    }
    if (center_index >= ring_count) {
      return LEME_WORKSPACE_SWIPE_IGNORED;
    }
    if (occupied) {
      initial_pos = (double)center_index;
    }
    center_pos =
        (double)center_index +
        (double)ring_count *
            round((initial_pos - (double)center_index) / (double)ring_count);
  }

  leme_session_notify_activity(server);

  server->gesture.mode = mode;
  server->gesture.active = true;
  server->gesture.engaged = is_takeover;
  server->gesture.pointer = event->pointer;
  server->gesture.output = output;
  server->gesture.natural_scroll = natural_scroll;
  server->gesture.initial_position = initial_pos;
  server->gesture.center_position = center_pos;
  server->gesture.initial_tag_id = focused_id;
  server->gesture.displacement = 0.0;
  server->gesture.dx_accum = 0.0;
  server->gesture.dy_accum = 0.0;
  memcpy(server->gesture.ring, ring, ring_count * sizeof(uint16_t));
  server->gesture.ring_count = ring_count;

  leme_swipe_tracker_init(
      &server->gesture.tracker,
      server->config->gestures.workspace_switch.velocity_window_ms,
      server->config->gestures.workspace_switch.deceleration);

  if (device != NULL && wlr_input_device_is_libinput(device)) {
    libinput_device = wlr_libinput_get_device_handle(device);
  }
  if (libinput_device != NULL) {
    (void)leme_gesture_accel_begin(&server->gesture.accel, libinput_device);
  }

  if (is_takeover) {
    leme_animation_manager_cancel_data(&server->animations,
                                       output->workspace_transition);
    leme_render_workspace_transition_begin_gesture(
        output->workspace_transition);
    /* Bounds were adopted before acquiring input/animation ownership. */
  }
  return is_takeover ? LEME_WORKSPACE_SWIPE_ENGAGED
                     : LEME_WORKSPACE_SWIPE_PENDING;
}

enum leme_workspace_swipe leme_input_workspace_swipe_update(
    struct leme_server *server,
    const struct wlr_pointer_swipe_update_event *event) {
  struct leme_output *output;
  const struct leme_workspace_switch_gesture_settings *settings;
  double effective_dx;

  if (server == NULL || !server->gesture.active || event == NULL) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }
  if (server->gesture.pointer != NULL && event->pointer != NULL &&
      event->pointer != server->gesture.pointer) {
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }
  if (!isfinite(event->dx) || !isfinite(event->dy)) {
    leme_input_workspace_gesture_cancel(server);
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  leme_session_notify_activity(server);
  output = server->gesture.output;
  if (output == NULL || output != leme_output_focused(server) ||
      leme_output_tags(output) == NULL || leme_session_locked(server)) {
    leme_input_workspace_gesture_cancel(server);
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  settings = &server->config->gestures.workspace_switch;
  if (settings->distance <= 0.0 || !isfinite(settings->distance)) {
    leme_input_workspace_gesture_cancel(server);
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  effective_dx = server->gesture.natural_scroll ? -event->dx : event->dx;

  if (!server->gesture.engaged) {
    server->gesture.dx_accum += effective_dx;
    server->gesture.dy_accum += event->dy;

    if (!isfinite(server->gesture.dx_accum) ||
        !isfinite(server->gesture.dy_accum)) {
      leme_input_workspace_gesture_cancel(server);
      return LEME_WORKSPACE_SWIPE_IGNORED;
    }

    const double dist =
        hypot(server->gesture.dx_accum, server->gesture.dy_accum);
    if (dist < 16.0) {
      return LEME_WORKSPACE_SWIPE_PENDING;
    }

    if (fabs(server->gesture.dy_accum) >= fabs(server->gesture.dx_accum)) {
      leme_input_workspace_gesture_reset(server);
      return LEME_WORKSPACE_SWIPE_DECLINED;
    }

    leme_swipe_tracker_reset(&server->gesture.tracker);
    server->gesture.displacement = 0.0;

    const size_t adjacent_index = (size_t)leme_tags_ring_wrap(
        server->gesture.center_position + 1.0, server->gesture.ring_count);
    const uint16_t adj = server->gesture.ring[adjacent_index];
    struct leme_workspace_transition *transition =
        leme_render_workspace_transition_prepare_gesture(
            output, server->gesture.initial_tag_id, adj,
            LEME_TAG_CHANGE_FORWARD);

    if (transition != NULL) {
      leme_render_workspace_transition_begin_gesture(transition);

      size_t count = 0;
      const uint16_t *ring =
          leme_render_workspace_transition_ring(output, &count);
      bool ring_matches = (ring != NULL && count == server->gesture.ring_count);
      if (ring_matches) {
        for (size_t i = 0; i < count; i++) {
          if (ring[i] != server->gesture.ring[i]) {
            ring_matches = false;
            break;
          }
        }
      }
      if (!ring_matches) {
        leme_render_workspace_transition_finish(output);
        leme_input_workspace_gesture_cancel(server);
        return LEME_WORKSPACE_SWIPE_IGNORED;
      }

      if (!leme_render_workspace_transition_set_gesture_range(
              output, server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE,
              server->gesture.center_position - 1.0,
              server->gesture.center_position + 1.0)) {
        leme_render_workspace_transition_finish(output);
        leme_input_workspace_gesture_cancel(server);
        return LEME_WORKSPACE_SWIPE_IGNORED;
      }
    }

    server->gesture.engaged = true;
  }

  const double delta = effective_dx / settings->distance;
  const double next_displacement = server->gesture.displacement + delta;
  const double raw = server->gesture.initial_position + next_displacement;

  if (!isfinite(delta) || !isfinite(next_displacement) || !isfinite(raw) ||
      (server->gesture.mode != LEME_WORKSPACE_GESTURE_SINGLE &&
       fabs(raw) > 0x1p52 - 2.0)) {
    leme_input_workspace_gesture_cancel(server);
    return LEME_WORKSPACE_SWIPE_IGNORED;
  }

  server->gesture.displacement = next_displacement;
  leme_swipe_tracker_push(&server->gesture.tracker, delta, event->time_msec);

  const double visual =
      server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE
          ? leme_swipe_position(raw, server->gesture.center_position)
          : raw;
  if (output->workspace_transition != NULL) {
    leme_render_workspace_transition_set_position(output, visual);
  }
  return LEME_WORKSPACE_SWIPE_ENGAGED;
}

void leme_input_workspace_swipe_end(
    struct leme_server *server,
    const struct wlr_pointer_swipe_end_event *event) {
  struct leme_output *output;
  const struct leme_workspace_switch_gesture_settings *settings;
  uint16_t target_id;
  size_t target_idx;
  enum leme_tag_change_direction change_dir;

  if (server == NULL || !server->gesture.active || event == NULL) {
    return;
  }
  if (server->gesture.pointer != NULL && event->pointer != NULL &&
      event->pointer != server->gesture.pointer) {
    return;
  }
  leme_session_notify_activity(server);
  output = server->gesture.output;
  if (output == NULL || output != leme_output_focused(server) ||
      leme_output_tags(output) == NULL || leme_session_locked(server)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (!server->gesture.engaged) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  settings = &server->config->gestures.workspace_switch;
  leme_swipe_tracker_push(&server->gesture.tracker, 0.0, event->time_msec);

  const double raw =
      server->gesture.initial_position + server->gesture.displacement;
  const bool single = server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE;
  double visual =
      single ? leme_swipe_position(raw, server->gesture.center_position) : raw;
  if (output->workspace_transition != NULL &&
      !leme_render_workspace_transition_presented_position(output, &visual)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }
  const double projection = leme_swipe_tracker_projected_position(
      &server->gesture.tracker, raw, event->time_msec);
  const double velocity =
      event->cancelled ? 0.0
                       : leme_swipe_tracker_velocity(&server->gesture.tracker,
                                                     event->time_msec) *
                             (single ? leme_swipe_position_derivative(
                                           raw, server->gesture.center_position)
                                     : 1.0);
  const bool forward = projection >= server->gesture.initial_position;
  bool bounded = single;
  double minimum = server->gesture.center_position - 1.0;
  double maximum = server->gesture.center_position + 1.0;
  double target = NAN;
  if (event->cancelled) {
    target = server->gesture.center_position;
    if (!single && server->gesture.ring_count >= 2) {
      const double count = (double)server->gesture.ring_count;
      target += count * round((visual - target) / count);
    }
  } else {
    switch (server->gesture.mode) {
    case LEME_WORKSPACE_GESTURE_SINGLE:
      target = leme_swipe_target(projection, server->gesture.center_position,
                                 settings->threshold);
      break;
    case LEME_WORKSPACE_GESTURE_SCRUB:
      bounded = true;
      minimum = floor(visual);
      maximum = ceil(visual);
      target =
          leme_swipe_continuous_target(fmax(minimum, fmin(projection, maximum)),
                                       settings->threshold, forward);
      break;
    case LEME_WORKSPACE_GESTURE_FREE:
      target = leme_swipe_continuous_target(projection, settings->threshold,
                                            forward);
      break;
    }
  }

  if (!isfinite(visual) || !isfinite(projection) || !isfinite(target) ||
      !isfinite(velocity)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (server->gesture.ring_count < 2) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  target_idx = (size_t)leme_tags_ring_wrap(target, server->gesture.ring_count);
  if (target_idx >= server->gesture.ring_count) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }
  target_id = server->gesture.ring[target_idx];

  if (output->workspace_transition != NULL &&
      !leme_render_workspace_transition_set_gesture_range(output, bounded,
                                                          minimum, maximum)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (target > server->gesture.center_position) {
    change_dir = LEME_TAG_CHANGE_FORWARD;
  } else if (target < server->gesture.center_position) {
    change_dir = LEME_TAG_CHANGE_BACKWARD;
  } else {
    change_dir = (visual >= server->gesture.center_position)
                     ? LEME_TAG_CHANGE_BACKWARD
                     : LEME_TAG_CHANGE_FORWARD;
  }

  (void)leme_tags_focus_id_direction(leme_output_tags(output), target_id,
                                     change_dir);

  struct leme_workspace_transition *transition = output->workspace_transition;
  leme_input_workspace_gesture_reset(server);
  leme_view_refresh_tag_focus(server);

  if (transition != NULL) {
    leme_render_workspace_transition_settle(output, visual, target, velocity);
  }
}
