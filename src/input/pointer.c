#include "input/pointer_internal.h"
#include "input/input.h"
#include "input/internal.h"
#include "public/server.h"

#include "core/server.h"
#include "desktop/desktop.h"
#include "desktop/drag.h"
#include "desktop/strip.h"
#include "desktop/buttons.h"
#include "desktop/owner.h"
#include "output/output.h"
#include "protocols/data.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "render/render.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void leme_input_follow_pointer_output(struct leme_server *server) {
  struct leme_output *output =
      leme_output_at(server, server->cursor->x, server->cursor->y);

  if (output == NULL || output == leme_output_focused(server)) {
    return;
  }
  leme_output_set_focused(server, output, false);
}

static enum leme_layout_kind
leme_input_pointer_view_layout(const struct leme_server *server,
                               const struct leme_view *view) {
  const struct leme_tag *tag = leme_ownership_tag(view);
  const struct leme_tags *tags = leme_focused_tags(server);

  return tag != NULL    ? tag->layout.kind
         : tags != NULL ? leme_tags_id_layout(tags, tags->focused_id)
                        : LEME_LAYOUT_DWINDLE;
}

static struct wlr_seat_client *
leme_input_surface_client(struct leme_server *server,
                          struct wlr_surface *surface) {
  return surface == NULL
             ? NULL
             : wlr_seat_client_for_wl_client(
                   server->seat, wl_resource_get_client(surface->resource));
}

void leme_input_process_motion(struct leme_server *server, uint32_t time_msec,
                               struct leme_render_hit *result,
                               bool allow_view_focus) {
  struct leme_render_hit local;
  struct leme_render_hit *hit = result == NULL ? &local : result;

  if (leme_desk_owner_handle_motion(server)) {
    *hit = (struct leme_render_hit){0};
    leme_input_pointer_resize_hover(server, hit);
    return;
  }
  leme_render_at(server, server->cursor->x, server->cursor->y, hit);
  if (!leme_session_surface_allowed(server, hit->surface)) {
    *hit = (struct leme_render_hit){0};
  }
  const bool entered = hit->view != server->hovered_view;
  const bool follow =
      allow_view_focus && !leme_session_locked(server) &&
      !wlr_seat_pointer_has_grab(server->seat) &&
      (server->config == NULL || server->config->focus_follows_mouse);

  server->hovered_view = hit->view;
  if (follow) {
    leme_input_follow_pointer_output(server);
  }
  struct leme_view *focus = hit->view != NULL        ? hit->view
                            : hit->container == NULL ? NULL
                                                     : hit->container->focused;
  if (entered && follow && leme_view_policy_hover_focus(focus)) {
    leme_view_focus_ex(focus, leme_input_pointer_view_layout(server, focus) !=
                                  LEME_LAYOUT_DESKTOP);
  }
  leme_desk_buttons_handle_motion(server, hit);
  if (hit->surface != server->seat->pointer_state.focused_surface &&
      server->seat->drag == NULL && !leme_input_pointer_grab_active(server) &&
      !leme_desktop_cursor_owned_by(
          server, leme_input_surface_client(server, hit->surface))) {
    leme_desktop_cursor_default(server);
    if (server->pointer_grab != NULL) {
      server->pointer_grab->hover_edges = LEME_GRAB_EDGE_NONE;
    }
  }
  leme_input_pointer_resize_hover(server, hit);
  if (hit->surface == NULL) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    leme_input_protocols_update_pointer_focus(server);
    return;
  }
  wlr_seat_pointer_notify_enter(server->seat, hit->surface, hit->surface_x,
                                hit->surface_y);
  wlr_seat_pointer_notify_motion(server->seat, time_msec, hit->surface_x,
                                 hit->surface_y);
  leme_input_protocols_update_pointer_focus(server);
}

void leme_input_refresh_pointer_focus(struct leme_server *server,
                                      uint32_t time_msec) {
  wlr_seat_pointer_notify_clear_focus(server->seat);
  leme_input_process_motion(server, time_msec, NULL, true);
}

static void leme_input_pointer_handle_rebase(void *data) {
  struct leme_server *server = data;

  server->pointer_rebase_idle = NULL;
  if (server->cursor == NULL || leme_input_pointer_grab_active(server) ||
      server->seat->drag != NULL || leme_session_locked(server) ||
      leme_input_protocols_constraint_active(server)) {
    return;
  }
  leme_input_process_motion(server, 0, NULL, false);
}

void leme_input_pointer_rebase(struct leme_server *server) {
  if (server->display == NULL || server->pointer_rebase_idle != NULL) {
    return;
  }
  server->pointer_rebase_idle =
      wl_event_loop_add_idle(wl_display_get_event_loop(server->display),
                             leme_input_pointer_handle_rebase, server);
}

static struct leme_pointer *
leme_input_pointer_find(struct leme_server *server,
                        const struct wlr_pointer *wlr_pointer) {
  struct leme_pointer *pointer;

  wl_list_for_each(pointer, &server->pointers, link) {
    if (pointer->device == &wlr_pointer->base) {
      return pointer;
    }
  }
  return NULL;
}

static bool leme_input_pointer_edge_mutes(struct leme_server *server,
                                          const struct wlr_pointer *wlr_pointer,
                                          enum leme_edge_event event,
                                          uint32_t button) {
  const struct leme_pointer *pointer =
      leme_input_pointer_find(server, wlr_pointer);

  return pointer != NULL && leme_input_edge_mutes(pointer->edge, event, button);
}

static void leme_input_handle_motion(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;

  double dx = event->delta_x;
  double dy = event->delta_y;

  leme_session_notify_activity(server);
  if (leme_input_pointer_edge_mutes(server, event->pointer,
                                    LEME_EDGE_EVENT_MOTION, 0)) {
    return;
  }
  if (leme_input_pointer_grab_active(server)) {
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec,
                                   server->cursor->x, server->cursor->y);
    return;
  }
  leme_input_protocols_send_relative(server, event->time_msec, dx, dy,
                                     event->unaccel_dx, event->unaccel_dy);
  if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
    return;
  }
  wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
  leme_data_update_drag_icon(server);
  leme_desk_drag_motion(server);
  leme_input_process_motion(server, event->time_msec, NULL, true);
}

static void leme_input_handle_motion_absolute(struct wl_listener *listener,
                                              void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  double layout_x;
  double layout_y;
  double dx;
  double dy;

  leme_session_notify_activity(server);
  wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base,
                                       event->x, event->y, &layout_x,
                                       &layout_y);
  dx = layout_x - server->cursor->x;
  dy = layout_y - server->cursor->y;
  if (leme_input_pointer_grab_active(server)) {
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec,
                                   server->cursor->x, server->cursor->y);
    return;
  }
  if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
    return;
  }
  wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
  leme_data_update_drag_icon(server);
  leme_desk_drag_motion(server);
  leme_input_process_motion(server, event->time_msec, NULL, true);
}

static void leme_input_handle_button(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  struct leme_render_hit hit;
  bool inhibited;

  leme_session_notify_activity(server);
  if (leme_input_pointer_edge_mutes(server, event->pointer,
                                    event->state ==
                                            WL_POINTER_BUTTON_STATE_PRESSED
                                        ? LEME_EDGE_EVENT_PRESS
                                        : LEME_EDGE_EVENT_RELEASE,
                                    event->button)) {
    return;
  }
  if (leme_desk_owner_handle_button(server, event)) {
    return;
  }
  if (leme_input_pointer_grab_active(server)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;
  }
  inhibited = leme_input_protocols_shortcuts_inhibited(server);
  leme_input_process_motion(server, event->time_msec, &hit, true);
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
      !leme_session_locked(server) && hit.container != NULL) {
    leme_view_focus(hit.container->focused);
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
      !leme_session_locked(server) && leme_view_policy_hover_focus(hit.view) &&
      (hit.view != server->focused_view ||
       leme_input_pointer_view_layout(server, hit.view) ==
           LEME_LAYOUT_DESKTOP)) {
    leme_view_focus(hit.view);
  }
  if (leme_input_pointer_try_compositor_grab(server, &hit, event, inhibited) ||
      leme_input_pointer_try_edge_resize(server, &hit, event)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (leme_desk_buttons_handle_button(server, &hit, event) ||
        leme_desk_handle_button(server, &hit, event) ||
        leme_desk_strip_handle_button(server, &hit, event)) {
      return;
    }
    leme_layer_focus_on_button(hit.layer);
  }
  wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
                                 event->state);
}

static void leme_input_handle_axis(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_axis);
  struct wlr_pointer_axis_event *event = data;

  leme_session_notify_activity(server);
  const struct leme_pointer *pointer =
      leme_input_pointer_find(server, event->pointer);
  if (pointer != NULL &&
      leme_input_edge_mutes(pointer->edge, LEME_EDGE_EVENT_AXIS, 0)) {
    return;
  }
  if (leme_desk_owner_handle_axis(server, event)) {
    return;
  }
  const double factor = pointer != NULL ? pointer->scroll_factor : 1.0;
  wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                               event->orientation, event->delta * factor,
                               (int32_t)lround(event->delta_discrete * factor),
                               event->source, event->relative_direction);
}

static void leme_input_handle_frame(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_frame);

  (void)data;
  wlr_seat_pointer_notify_frame(server->seat);
}

static void leme_input_handle_set_cursor(struct wl_listener *listener,
                                         void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, request_set_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;

  if (!leme_input_pointer_grab_active(server) &&
      !leme_input_protocols_pointer_locked(server) &&
      leme_session_surface_allowed(
          server, server->seat->pointer_state.focused_surface) &&
      server->seat->pointer_state.focused_client == event->seat_client) {
    leme_desktop_cursor_surface_set(server, event->seat_client);
    wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

static void leme_input_handle_swipe_begin(struct wl_listener *listener,
                                          void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_begin);
  const struct wlr_pointer_swipe_begin_event *event = data;

  if (!leme_input_pointer_edge_mutes(server, event->pointer,
                                     LEME_EDGE_EVENT_GESTURE, 0)) {
    leme_input_gesture_swipe_begin(server, event);
  }
}

static void leme_input_handle_swipe_update(struct wl_listener *listener,
                                           void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_update);

  leme_input_gesture_swipe_update(server, data);
}

static void leme_input_handle_swipe_end(struct wl_listener *listener,
                                        void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_end);

  leme_input_gesture_swipe_end(server, data);
}

static void leme_input_handle_pinch_begin(struct wl_listener *listener,
                                          void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_pinch_begin);
  const struct wlr_pointer_pinch_begin_event *event = data;

  if (!leme_input_pointer_edge_mutes(server, event->pointer,
                                     LEME_EDGE_EVENT_GESTURE, 0)) {
    leme_input_gesture_pinch_begin(server, event);
  }
}

static void leme_input_handle_pinch_update(struct wl_listener *listener,
                                           void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_pinch_update);

  leme_input_gesture_pinch_update(server, data);
}

static void leme_input_handle_pinch_end(struct wl_listener *listener,
                                        void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_pinch_end);

  leme_input_gesture_pinch_end(server, data);
}

static void leme_input_handle_hold_begin(struct wl_listener *listener,
                                         void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_hold_begin);
  const struct wlr_pointer_hold_begin_event *event = data;

  if (!leme_input_pointer_edge_mutes(server, event->pointer,
                                     LEME_EDGE_EVENT_GESTURE, 0)) {
    leme_input_gesture_hold_begin(server, event);
  }
}

static void leme_input_handle_hold_end(struct wl_listener *listener,
                                       void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_hold_end);

  leme_input_gesture_hold_end(server, data);
}

void leme_input_pointer_events_init(struct leme_server *server) {
  struct leme_pointer_grab *grab = calloc(1, sizeof(*grab));

  if (grab == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to allocate pointer grab");
  } else {
    grab->server = server;
    grab->seat_grab.interface = &leme_pointer_grab_interface;
    grab->seat_grab.data = grab;
    server->pointer_grab = grab;
  }
  server->cursor_motion.notify = leme_input_handle_motion;
  wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
  server->cursor_motion_absolute.notify = leme_input_handle_motion_absolute;
  wl_signal_add(&server->cursor->events.motion_absolute,
                &server->cursor_motion_absolute);
  server->cursor_button.notify = leme_input_handle_button;
  wl_signal_add(&server->cursor->events.button, &server->cursor_button);
  server->cursor_axis.notify = leme_input_handle_axis;
  wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
  server->cursor_frame.notify = leme_input_handle_frame;
  wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
  server->cursor_swipe_begin.notify = leme_input_handle_swipe_begin;
  wl_signal_add(&server->cursor->events.swipe_begin,
                &server->cursor_swipe_begin);
  server->cursor_swipe_update.notify = leme_input_handle_swipe_update;
  wl_signal_add(&server->cursor->events.swipe_update,
                &server->cursor_swipe_update);
  server->cursor_swipe_end.notify = leme_input_handle_swipe_end;
  wl_signal_add(&server->cursor->events.swipe_end, &server->cursor_swipe_end);
  server->cursor_pinch_begin.notify = leme_input_handle_pinch_begin;
  wl_signal_add(&server->cursor->events.pinch_begin,
                &server->cursor_pinch_begin);
  server->cursor_pinch_update.notify = leme_input_handle_pinch_update;
  wl_signal_add(&server->cursor->events.pinch_update,
                &server->cursor_pinch_update);
  server->cursor_pinch_end.notify = leme_input_handle_pinch_end;
  wl_signal_add(&server->cursor->events.pinch_end, &server->cursor_pinch_end);
  server->cursor_hold_begin.notify = leme_input_handle_hold_begin;
  wl_signal_add(&server->cursor->events.hold_begin, &server->cursor_hold_begin);
  server->cursor_hold_end.notify = leme_input_handle_hold_end;
  wl_signal_add(&server->cursor->events.hold_end, &server->cursor_hold_end);
  server->request_set_cursor.notify = leme_input_handle_set_cursor;
  wl_signal_add(&server->seat->events.request_set_cursor,
                &server->request_set_cursor);
}

void leme_input_pointers_finish(struct leme_server *server) {
  leme_public_server_invalidate(server);
  struct leme_pointer *pointer;
  struct leme_pointer *temporary;

  leme_input_workspace_gesture_cancel(server);
  leme_input_pointer_grab_finish(server);
  free(server->pointer_grab);
  server->pointer_grab = NULL;
  wl_list_for_each_safe(pointer, temporary, &server->pointers, link) {
    wlr_cursor_detach_input_device(server->cursor, pointer->device);
    wl_list_remove(&pointer->destroy.link);
    wl_list_remove(&pointer->link);
    leme_input_edge_reader_destroy(pointer->edge);
    free(pointer);
  }
  wl_list_remove(&server->cursor_motion.link);
  wl_list_remove(&server->cursor_motion_absolute.link);
  wl_list_remove(&server->cursor_button.link);
  wl_list_remove(&server->cursor_axis.link);
  wl_list_remove(&server->cursor_frame.link);
  wl_list_remove(&server->cursor_swipe_begin.link);
  wl_list_remove(&server->cursor_pinch_begin.link);
  wl_list_remove(&server->cursor_pinch_update.link);
  wl_list_remove(&server->cursor_pinch_end.link);
  wl_list_remove(&server->cursor_hold_begin.link);
  wl_list_remove(&server->cursor_hold_end.link);
  wl_list_remove(&server->cursor_swipe_update.link);
  wl_list_remove(&server->cursor_swipe_end.link);
  wl_list_remove(&server->request_set_cursor.link);
}
