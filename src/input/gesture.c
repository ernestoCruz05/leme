#include "core/command.h"
#include "core/server.h"
#include "input/input.h"
#include "input/internal.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <math.h>
#include <stdint.h>
#include <time.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_seat.h>

#define LEME_GESTURE_DECIDE_DISTANCE 16.0
#define LEME_GESTURE_SWIPE_DISTANCE 100.0
#define LEME_GESTURE_PINCH_DECIDE 0.1
#define LEME_GESTURE_PINCH_FIRE 0.25
#define LEME_GESTURE_ROTATE_DECIDE 10.0

const struct leme_binding *leme_input_gesture_binding(
    const struct leme_server *server, enum leme_binding_trigger trigger,
    uint32_t fingers, uint32_t modifiers, enum leme_gesture_direction direction,
    enum leme_edge_side edge) {
  const struct leme_mode *mode = server->active_mode;

  for (size_t i = 0; mode != NULL && i < mode->binding_count; i++) {
    const struct leme_binding *binding = &mode->bindings[i];

    if (binding->trigger == trigger && binding->fingers == fingers &&
        binding->modifiers == modifiers && binding->direction == direction &&
        binding->edge == edge) {
      return binding;
    }
  }
  return NULL;
}

static bool leme_input_gesture_candidate(const struct leme_server *server,
                                         enum leme_binding_trigger trigger,
                                         uint32_t fingers, uint32_t modifiers) {
  const struct leme_mode *mode = server->active_mode;

  for (size_t i = 0; mode != NULL && i < mode->binding_count; i++) {
    const struct leme_binding *binding = &mode->bindings[i];

    if (binding->trigger == trigger && binding->fingers == fingers &&
        binding->modifiers == modifiers &&
        binding->direction != LEME_GESTURE_NONE) {
      return true;
    }
  }
  return false;
}

void leme_input_gesture_run(struct leme_server *server,
                            const struct leme_binding *binding) {
  if (leme_session_command_allowed(server, &binding->command)) {
    (void)leme_command_execute(server, &binding->command);
  }
}

static bool leme_input_gesture_owns(const struct leme_server *server,
                                    enum leme_binding_trigger kind,
                                    const struct wlr_pointer *pointer) {
  const struct leme_gesture_state *state = &server->gesture_route;

  return state->route != LEME_GESTURE_ROUTE_IDLE && state->kind == kind &&
         (state->pointer == NULL || pointer == NULL ||
          state->pointer == pointer);
}

static void leme_input_gesture_start(struct leme_server *server,
                                     enum leme_binding_trigger kind,
                                     uint32_t fingers, uint32_t time_msec,
                                     struct wlr_pointer *pointer) {
  leme_input_gesture_cancel(server);
  leme_session_notify_activity(server);
  server->gesture_route = (struct leme_gesture_state){
      .route = LEME_GESTURE_ROUTE_FORWARD,
      .kind = kind,
      .fingers = fingers,
      .modifiers = leme_input_modifiers(server),
      .begin_msec = time_msec,
      .bindings = !leme_input_protocols_shortcuts_inhibited(server) &&
                  !leme_session_locked(server) &&
                  !leme_input_pointer_grab_active(server) &&
                  server->seat->drag == NULL,
      .scale = 1.0,
      .pointer = pointer,
  };
}

void leme_input_gesture_cancel(struct leme_server *server) {
  struct leme_gesture_state *state = &server->gesture_route;
  struct timespec now = {0};

  switch (state->route) {
  case LEME_GESTURE_ROUTE_IDLE:
    return;
  case LEME_GESTURE_ROUTE_PENDING:
  case LEME_GESTURE_ROUTE_WORKSPACE:
    if (state->workspace) {
      leme_input_workspace_gesture_cancel(server);
    }
    break;
  case LEME_GESTURE_ROUTE_BINDING:
    break;
  case LEME_GESTURE_ROUTE_GRAB:
    leme_input_pointer_gesture_grab_end(server, true);
    break;
  case LEME_GESTURE_ROUTE_FORWARD: {
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t time_msec = (uint32_t)((uint64_t)now.tv_sec * 1000u +
                                          (uint64_t)now.tv_nsec / 1000000u);
    switch (state->kind) {
    case LEME_BINDING_SWIPE:
      wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures,
                                             server->seat, time_msec, true);
      break;
    case LEME_BINDING_PINCH:
      wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures,
                                             server->seat, time_msec, true);
      break;
    case LEME_BINDING_HOLD:
      wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures,
                                            server->seat, time_msec, true);
      break;
    case LEME_BINDING_KEY:
    case LEME_BINDING_EDGE:
      break;
    }
    break;
  }
  }
  server->gesture_route = (struct leme_gesture_state){0};
}

void leme_input_gesture_swipe_begin(
    struct leme_server *server,
    const struct wlr_pointer_swipe_begin_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  leme_input_gesture_start(server, LEME_BINDING_SWIPE, event->fingers,
                           event->time_msec, event->pointer);
  const struct leme_binding *grab =
      state->bindings
          ? leme_input_gesture_binding(server, LEME_BINDING_SWIPE,
                                       event->fingers, state->modifiers,
                                       LEME_GESTURE_NONE, LEME_EDGE_NONE)
          : NULL;
  if (grab != NULL) {
    if (leme_input_pointer_gesture_grab_begin(
            server, grab->command.type == LEME_COMMAND_GESTURE_RESIZE)) {
      state->route = LEME_GESTURE_ROUTE_GRAB;
    } else {
      wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures,
                                               server->seat, event->time_msec,
                                               event->fingers);
    }
    return;
  }
  state->bindings = state->bindings && leme_input_gesture_candidate(
                                           server, LEME_BINDING_SWIPE,
                                           event->fingers, state->modifiers);
  switch (leme_input_workspace_swipe_begin(server, event)) {
  case LEME_WORKSPACE_SWIPE_IGNORED:
  case LEME_WORKSPACE_SWIPE_DECLINED:
    if (state->bindings) {
      state->route = LEME_GESTURE_ROUTE_PENDING;
    } else {
      wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures,
                                               server->seat, event->time_msec,
                                               event->fingers);
    }
    break;
  case LEME_WORKSPACE_SWIPE_PENDING:
    state->route = LEME_GESTURE_ROUTE_PENDING;
    state->workspace = true;
    break;
  case LEME_WORKSPACE_SWIPE_ENGAGED:
    state->route = LEME_GESTURE_ROUTE_WORKSPACE;
    state->workspace = true;
    break;
  }
}

void leme_input_gesture_swipe_update(
    struct leme_server *server,
    const struct wlr_pointer_swipe_update_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  if (!leme_input_gesture_owns(server, LEME_BINDING_SWIPE, event->pointer)) {
    return;
  }
  leme_session_notify_activity(server);
  switch (state->route) {
  case LEME_GESTURE_ROUTE_IDLE:
    return;
  case LEME_GESTURE_ROUTE_FORWARD:
    wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures,
                                              server->seat, event->time_msec,
                                              event->dx, event->dy);
    return;
  case LEME_GESTURE_ROUTE_WORKSPACE:
    (void)leme_input_workspace_swipe_update(server, event);
    return;
  case LEME_GESTURE_ROUTE_BINDING:
    state->dx += event->dx;
    state->dy += event->dy;
    return;
  case LEME_GESTURE_ROUTE_GRAB:
    leme_input_pointer_gesture_grab_motion(server, event->time_msec, event->dx,
                                           event->dy);
    return;
  case LEME_GESTURE_ROUTE_PENDING:
    state->dx += event->dx;
    state->dy += event->dy;
    break;
  }
  if (state->workspace) {
    switch (leme_input_workspace_swipe_update(server, event)) {
    case LEME_WORKSPACE_SWIPE_PENDING:
      return;
    case LEME_WORKSPACE_SWIPE_ENGAGED:
      state->route = LEME_GESTURE_ROUTE_WORKSPACE;
      return;
    case LEME_WORKSPACE_SWIPE_IGNORED:
    case LEME_WORKSPACE_SWIPE_DECLINED:
      state->workspace = false;
      break;
    }
  }
  if (hypot(state->dx, state->dy) < LEME_GESTURE_DECIDE_DISTANCE) {
    return;
  }
  const enum leme_gesture_direction direction =
      fabs(state->dy) >= fabs(state->dx)
          ? (state->dy < 0.0 ? LEME_GESTURE_UP : LEME_GESTURE_DOWN)
          : (state->dx < 0.0 ? LEME_GESTURE_LEFT : LEME_GESTURE_RIGHT);
  state->binding = state->bindings
                       ? leme_input_gesture_binding(
                             server, LEME_BINDING_SWIPE, state->fingers,
                             state->modifiers, direction, LEME_EDGE_NONE)
                       : NULL;
  if (state->binding != NULL) {
    state->route = LEME_GESTURE_ROUTE_BINDING;
    return;
  }
  state->route = LEME_GESTURE_ROUTE_FORWARD;
  wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures,
                                           server->seat, state->begin_msec,
                                           state->fingers);
  wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures,
                                            server->seat, event->time_msec,
                                            state->dx, state->dy);
}

void leme_input_gesture_swipe_end(
    struct leme_server *server,
    const struct wlr_pointer_swipe_end_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  if (!leme_input_gesture_owns(server, LEME_BINDING_SWIPE, event->pointer)) {
    return;
  }
  leme_session_notify_activity(server);
  switch (state->route) {
  case LEME_GESTURE_ROUTE_IDLE:
    break;
  case LEME_GESTURE_ROUTE_GRAB:
    leme_input_pointer_gesture_grab_end(server, event->cancelled);
    break;
  case LEME_GESTURE_ROUTE_FORWARD:
    wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures,
                                           server->seat, event->time_msec,
                                           event->cancelled);
    break;
  case LEME_GESTURE_ROUTE_PENDING:
  case LEME_GESTURE_ROUTE_WORKSPACE:
    if (state->workspace) {
      leme_input_workspace_swipe_end(server, event);
    }
    break;
  case LEME_GESTURE_ROUTE_BINDING: {
    const double travel =
        state->binding->direction == LEME_GESTURE_UP     ? -state->dy
        : state->binding->direction == LEME_GESTURE_DOWN ? state->dy
        : state->binding->direction == LEME_GESTURE_LEFT ? -state->dx
                                                         : state->dx;
    if (!event->cancelled && travel >= LEME_GESTURE_SWIPE_DISTANCE) {
      leme_input_gesture_run(server, state->binding);
    }
    break;
  }
  }
  server->gesture_route = (struct leme_gesture_state){0};
}

void leme_input_gesture_pinch_begin(
    struct leme_server *server,
    const struct wlr_pointer_pinch_begin_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  leme_input_gesture_start(server, LEME_BINDING_PINCH, event->fingers,
                           event->time_msec, event->pointer);
  if (state->bindings &&
      leme_input_gesture_candidate(server, LEME_BINDING_PINCH, event->fingers,
                                   state->modifiers)) {
    state->route = LEME_GESTURE_ROUTE_PENDING;
    return;
  }
  wlr_pointer_gestures_v1_send_pinch_begin(
      server->pointer_gestures, server->seat, event->time_msec, event->fingers);
}

void leme_input_gesture_pinch_update(
    struct leme_server *server,
    const struct wlr_pointer_pinch_update_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  if (!leme_input_gesture_owns(server, LEME_BINDING_PINCH, event->pointer)) {
    return;
  }
  leme_session_notify_activity(server);
  state->scale = event->scale;
  if (state->route == LEME_GESTURE_ROUTE_FORWARD) {
    wlr_pointer_gestures_v1_send_pinch_update(
        server->pointer_gestures, server->seat, event->time_msec, event->dx,
        event->dy, event->scale, event->rotation);
    return;
  }
  if (state->route != LEME_GESTURE_ROUTE_PENDING) {
    return;
  }
  state->dx += event->dx;
  state->dy += event->dy;
  state->rotation += event->rotation;
  const bool scaled = fabs(state->scale - 1.0) >= LEME_GESTURE_PINCH_DECIDE;
  if (!scaled && fabs(state->rotation) < LEME_GESTURE_ROTATE_DECIDE) {
    return;
  }
  state->binding =
      scaled ? leme_input_gesture_binding(
                   server, LEME_BINDING_PINCH, state->fingers, state->modifiers,
                   state->scale < 1.0 ? LEME_GESTURE_IN : LEME_GESTURE_OUT,
                   LEME_EDGE_NONE)
             : NULL;
  if (state->binding != NULL) {
    state->route = LEME_GESTURE_ROUTE_BINDING;
    return;
  }
  state->route = LEME_GESTURE_ROUTE_FORWARD;
  wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures,
                                           server->seat, state->begin_msec,
                                           state->fingers);
  wlr_pointer_gestures_v1_send_pinch_update(
      server->pointer_gestures, server->seat, event->time_msec, state->dx,
      state->dy, state->scale, state->rotation);
}

void leme_input_gesture_pinch_end(
    struct leme_server *server,
    const struct wlr_pointer_pinch_end_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  if (!leme_input_gesture_owns(server, LEME_BINDING_PINCH, event->pointer)) {
    return;
  }
  leme_session_notify_activity(server);
  if (state->route == LEME_GESTURE_ROUTE_FORWARD) {
    wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures,
                                           server->seat, event->time_msec,
                                           event->cancelled);
  } else if (state->route == LEME_GESTURE_ROUTE_BINDING && !event->cancelled &&
             (state->binding->direction == LEME_GESTURE_IN
                  ? 1.0 - state->scale
                  : state->scale - 1.0) >= LEME_GESTURE_PINCH_FIRE) {
    leme_input_gesture_run(server, state->binding);
  }
  server->gesture_route = (struct leme_gesture_state){0};
}

void leme_input_gesture_hold_begin(
    struct leme_server *server,
    const struct wlr_pointer_hold_begin_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  leme_input_gesture_start(server, LEME_BINDING_HOLD, event->fingers,
                           event->time_msec, event->pointer);
  state->binding =
      state->bindings
          ? leme_input_gesture_binding(server, LEME_BINDING_HOLD,
                                       event->fingers, state->modifiers,
                                       LEME_GESTURE_NONE, LEME_EDGE_NONE)
          : NULL;
  if (state->binding != NULL) {
    state->route = LEME_GESTURE_ROUTE_BINDING;
    return;
  }
  wlr_pointer_gestures_v1_send_hold_begin(
      server->pointer_gestures, server->seat, event->time_msec, event->fingers);
}

void leme_input_gesture_hold_end(
    struct leme_server *server,
    const struct wlr_pointer_hold_end_event *event) {
  struct leme_gesture_state *state = &server->gesture_route;

  if (!leme_input_gesture_owns(server, LEME_BINDING_HOLD, event->pointer)) {
    return;
  }
  leme_session_notify_activity(server);
  if (state->route == LEME_GESTURE_ROUTE_FORWARD) {
    wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures,
                                          server->seat, event->time_msec,
                                          event->cancelled);
  } else if (!event->cancelled) {
    leme_input_gesture_run(server, state->binding);
  }
  server->gesture_route = (struct leme_gesture_state){0};
}
