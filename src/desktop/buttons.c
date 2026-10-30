#include "desktop/buttons.h"

#include "core/server.h"
#include "desktop/decoration.h"
#include "desktop/menu.h"
#include "desktop/owner.h"
#include "desktop/target.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/input.h"
#include "shell/container.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

struct leme_desk_buttons {
  struct leme_server *server;
  struct leme_desk_target target;
  struct leme_desk_target hover;
  enum leme_render_decoration_part hover_part;
  bool hover_pressed;
  struct leme_output *output;
  uint16_t tag;
  enum leme_render_decoration_part part;
  double x;
  double y;
  struct leme_desk_target click_target;
  struct leme_output *click_output;
  uint16_t click_tag;
  uint32_t click_time;
  double click_x;
  double click_y;
};

static const struct leme_desk_owner leme_desk_buttons_owner;

static struct leme_desk_target
leme_desk_buttons_hit(const struct leme_render_hit *hit) {
  if (hit != NULL && hit->container != NULL) {
    return (struct leme_desk_target){LEME_DESK_TARGET_CONTAINER,
                                     hit->container->id};
  }
  return (struct leme_desk_target){LEME_DESK_TARGET_VIEW,
                                   hit != NULL && hit->view != NULL
                                       ? hit->view->public_meta.id
                                       : (struct leme_public_id){0}};
}

static struct leme_tag *leme_desk_buttons_tag(struct leme_server *server,
                                              struct leme_desk_target target) {
  struct leme_container *container = leme_desk_target_container(server, target);
  return container != NULL
             ? container->tag
             : leme_ownership_tag(leme_desk_target_view(server, target));
}

static bool leme_desk_buttons_focused(struct leme_server *server,
                                      struct leme_desk_target target) {
  if (target.kind == LEME_DESK_TARGET_CONTAINER) {
    struct leme_container *container =
        leme_desk_target_container(server, target);
    return container != NULL &&
           leme_container_for_view(server->focused_view) == container;
  }
  struct leme_view *view = leme_desk_target_view(server, target);
  return view != NULL && view == server->focused_view;
}

bool leme_desk_buttons_available(const struct leme_view *view,
                                 enum leme_render_decoration_part part) {
  if (!leme_desk_decoration_eligible(view) || leme_view_is_minimized(view)) {
    return false;
  }
  switch (part) {
  case LEME_RENDER_DECORATION_MINIMIZE:
    return leme_view_minimize_available(view);
  case LEME_RENDER_DECORATION_MAXIMIZE:
    return leme_view_is_maximized(view) || leme_view_can_maximize(view);
  case LEME_RENDER_DECORATION_CLOSE:
    return true;
  default:
    return false;
  }
}

static bool
leme_desk_buttons_target_available(struct leme_server *server,
                                   struct leme_desk_target target,
                                   enum leme_render_decoration_part part) {
  struct leme_container *container = leme_desk_target_container(server, target);
  if (container != NULL) {
    return !container->minimized &&
           ((part == LEME_RENDER_DECORATION_MINIMIZE &&
             leme_container_minimize_available(container)) ||
            part == LEME_RENDER_DECORATION_CLOSE ||
            (part == LEME_RENDER_DECORATION_MAXIMIZE &&
             leme_container_can_place(container,
                                      LEME_VIEW_PLACEMENT_MAXIMIZED)));
  }
  return leme_desk_buttons_available(leme_desk_target_view(server, target),
                                     part);
}

static void leme_desk_buttons_target_feedback(
    struct leme_server *server, struct leme_desk_target target,
    enum leme_render_decoration_part part, bool pressed) {
  struct leme_container *container = leme_desk_target_container(server, target);
  if (container != NULL) {
    leme_desk_frame_feedback(&container->frame, part, pressed);
  } else {
    struct leme_view *view = leme_desk_target_view(server, target);
    if (view != NULL) {
      leme_desk_decoration_feedback(view, part, pressed);
    }
  }
}

static void leme_desk_buttons_feedback(struct leme_desk_buttons *state,
                                       struct leme_desk_target hover,
                                       enum leme_render_decoration_part part,
                                       bool pressed) {
  if (leme_desk_target_equal(state->hover, hover) &&
      state->hover_part == part && state->hover_pressed == pressed) {
    return;
  }
  if (!leme_desk_target_equal(state->hover, hover)) {
    leme_desk_buttons_target_feedback(state->server, state->hover,
                                      LEME_RENDER_DECORATION_NONE, false);
  }
  state->hover = hover;
  state->hover_part = part;
  state->hover_pressed = pressed;
  leme_desk_buttons_target_feedback(state->server, hover, part, pressed);
}

void leme_desk_buttons_cancel(struct leme_server *server) {
  if (server == NULL) {
    return;
  }
  leme_desk_owner_release(server, &leme_desk_buttons_owner);
  struct leme_desk_buttons *state = server->desk_buttons;
  if (state == NULL) {
    return;
  }
  leme_desk_buttons_feedback(state, (struct leme_desk_target){0},
                             LEME_RENDER_DECORATION_NONE, false);
  state->target = (struct leme_desk_target){0};
  state->part = LEME_RENDER_DECORATION_NONE;
  state->output = NULL;
  state->click_target = (struct leme_desk_target){0};
  state->click_output = NULL;
}

static void
leme_desk_buttons_invalidate_target(struct leme_server *server,
                                    struct leme_desk_target target) {
  leme_desk_menu_invalidate(server, target);
  const struct leme_desk_buttons *state =
      server == NULL ? NULL : server->desk_buttons;
  if (state != NULL && (leme_desk_target_equal(state->target, target) ||
                        leme_desk_target_equal(state->hover, target) ||
                        leme_desk_target_equal(state->click_target, target))) {
    leme_desk_buttons_cancel(server);
  }
}

void leme_desk_buttons_invalidate_view(struct leme_view *view) {
  if (view != NULL) {
    leme_desk_buttons_invalidate_target(
        view->server,
        (struct leme_desk_target){LEME_DESK_TARGET_VIEW, view->public_meta.id});
  }
}

void leme_desk_buttons_invalidate_container(struct leme_container *container) {
  if (container != NULL) {
    leme_desk_buttons_invalidate_target(
        container->tag->owner->server,
        (struct leme_desk_target){LEME_DESK_TARGET_CONTAINER, container->id});
  }
}

static struct leme_desk_buttons *
leme_desk_buttons_state(struct leme_server *server) {
  if (server->desk_buttons == NULL) {
    server->desk_buttons = calloc(1, sizeof(*server->desk_buttons));
    if (server->desk_buttons != NULL) {
      server->desk_buttons->server = server;
    }
  }
  return server->desk_buttons;
}

static bool leme_desk_buttons_allowed(struct leme_server *server) {
  if (server == NULL || !server->desk_decorations_ready ||
      server->seat == NULL || server->cursor == NULL ||
      !leme_input_idle(server) ||
      leme_input_protocols_shortcuts_inhibited(server) ||
      server->seat->pointer_state.button_count != 0 ||
      (server->desk_owner.owner != NULL &&
       server->desk_owner.owner != &leme_desk_buttons_owner)) {
    return false;
  }
  const uint32_t group =
      server->config == NULL ? 0 : server->config->desktop_container_modifier;
  return (leme_input_modifiers(server) & ~group) == 0;
}

static bool leme_desk_buttons_visible(struct leme_server *server,
                                      struct leme_desk_target target) {
  const struct leme_tag *tag = leme_desk_buttons_tag(server, target);
  const struct leme_output *output =
      tag == NULL || tag->owner == NULL ? NULL : tag->owner->output;
  struct leme_container *container = leme_desk_target_container(server, target);
  const struct leme_view *view = leme_desk_target_view(server, target);
  struct wlr_scene_tree *tree = NULL;
  if (container != NULL && !container->minimized) {
    tree = container->render_tree;
  } else if (leme_desk_decoration_eligible(view) &&
             !leme_view_is_minimized(view) &&
             leme_desk_decoration_height(view, view->box) != 0) {
    tree = view->render_tree;
  }
  int x = 0;
  int y = 0;
  return output != NULL && output->wlr_output != NULL &&
         output->wlr_output->enabled && !output->tags.focused_is_candidate &&
         output->tags.focused_id == tag->id && tree != NULL &&
         wlr_scene_node_coords(&tree->node, &x, &y);
}

void leme_desk_buttons_handle_motion(struct leme_server *server,
                                     const struct leme_render_hit *hit) {
  if (!leme_desk_buttons_allowed(server)) {
    leme_desk_buttons_cancel(server);
    return;
  }
  const struct leme_desk_target actual = leme_desk_buttons_hit(hit);
  if (server->desk_buttons == NULL &&
      (!leme_desk_buttons_visible(server, actual) ||
       hit->decoration_part == LEME_RENDER_DECORATION_NONE ||
       leme_desk_buttons_state(server) == NULL)) {
    return;
  }
  struct leme_desk_buttons *state = server->desk_buttons;
  const struct leme_tag *tag = leme_desk_buttons_tag(server, state->target);
  if (state->target.id.serial != 0 &&
      (!leme_desk_buttons_visible(server, state->target) ||
       !leme_desk_buttons_focused(server, state->target) || tag == NULL ||
       tag->owner->output != state->output ||
       state->output != server->focused_output || tag->id != state->tag ||
       !leme_desk_buttons_target_available(server, state->target,
                                           state->part) ||
       hypot(server->cursor->x - state->x, server->cursor->y - state->y) >
           4.0)) {
    leme_desk_buttons_cancel(server);
  }
  struct leme_desk_target hover = {0};
  enum leme_render_decoration_part part = LEME_RENDER_DECORATION_NONE;
  bool pressed = false;
  if (leme_desk_buttons_visible(server, actual) &&
      hit->decoration_part != LEME_RENDER_DECORATION_NONE) {
    hover = actual;
    part = hit->decoration_part;
    pressed = server->desk_owner.owner == &leme_desk_buttons_owner &&
              leme_desk_target_equal(state->target, hover) &&
              state->part == part;
  }
  leme_desk_buttons_feedback(state, hover, part, pressed);
}

bool leme_desk_buttons_handle_button(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event) {
  if (server == NULL || event == NULL ||
      event->state != WL_POINTER_BUTTON_STATE_PRESSED) {
    return false;
  }
  if (event->button != BTN_LEFT) {
    leme_desk_buttons_cancel(server);
    const struct leme_desk_target target = leme_desk_buttons_hit(hit);
    if (event->button != BTN_RIGHT || hit == NULL ||
        hit->decoration_part == LEME_RENDER_DECORATION_NONE ||
        !leme_desk_buttons_allowed(server) ||
        !leme_desk_buttons_visible(server, target) ||
        leme_input_modifiers(server) != 0) {
      return false;
    }
    struct leme_render_hit current = {0};
    (void)leme_render_at(server, server->cursor->x, server->cursor->y,
                         &current);
    return leme_desk_target_equal(leme_desk_buttons_hit(&current), target) &&
           current.decoration_part != LEME_RENDER_DECORATION_NONE &&
           leme_desk_menu_open(server, target, server->cursor->x,
                               server->cursor->y);
  }
  const struct leme_desk_target target = leme_desk_buttons_hit(hit);
  if (hit == NULL || hit->decoration_part == LEME_RENDER_DECORATION_NONE ||
      !leme_desk_buttons_allowed(server) ||
      !leme_desk_buttons_visible(server, target)) {
    leme_desk_buttons_cancel(server);
    return false;
  }
  struct leme_render_hit current = {0};
  (void)leme_render_at(server, server->cursor->x, server->cursor->y, &current);
  struct leme_desk_buttons *state = leme_desk_buttons_state(server);
  if (!leme_desk_target_equal(leme_desk_buttons_hit(&current), target) ||
      current.decoration_part != hit->decoration_part || state == NULL) {
    return false;
  }
  if (!leme_desk_buttons_focused(server, target)) {
    leme_view_focus(leme_desk_target_member(server, target));
  }
  if (!leme_desk_buttons_focused(server, target)) {
    return false;
  }
  const struct leme_tag *tag = leme_desk_buttons_tag(server, target);
  if (hit->decoration_part == LEME_RENDER_DECORATION_TITLEBAR) {
    const bool double_click =
        leme_desk_target_equal(state->click_target, target) &&
        state->click_output == tag->owner->output &&
        state->click_tag == tag->id &&
        event->time_msec - state->click_time <= 400U &&
        hypot(server->cursor->x - state->click_x,
              server->cursor->y - state->click_y) <= 4.0;
    leme_desk_buttons_cancel(server);
    if (double_click) {
      leme_desk_owner_swallow(server, BTN_LEFT);
      leme_desk_target_act(server, target, LEME_RENDER_DECORATION_MAXIMIZE);
      return true;
    }
    const bool started =
        target.kind == LEME_DESK_TARGET_CONTAINER
            ? leme_input_pointer_grab_start_container(
                  leme_desk_target_container(server, target), event->button)
            : leme_input_pointer_grab_start_decoration(
                  leme_desk_target_view(server, target), event->button);
    if (started) {
      state->click_target = target;
      state->click_output = tag->owner->output;
      state->click_tag = tag->id;
      state->click_time = event->time_msec;
      state->click_x = server->cursor->x;
      state->click_y = server->cursor->y;
      wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                     event->button, event->state);
      return true;
    }
  }
  leme_desk_buttons_cancel(server);
  leme_desk_owner_claim(server, &leme_desk_buttons_owner, BTN_LEFT);
  state->x = server->cursor->x;
  state->y = server->cursor->y;
  state->output = tag->owner->output;
  state->tag = tag->id;
  state->part = hit->decoration_part;
  state->target = target;
  (void)leme_render_at(server, server->cursor->x, server->cursor->y, &current);
  leme_desk_buttons_handle_motion(server, &current);
  return true;
}

static bool
leme_desk_buttons_release(struct leme_server *server,
                          const struct wlr_pointer_button_event *event) {
  struct leme_desk_buttons *state = server->desk_buttons;
  if (event->state != WL_POINTER_BUTTON_STATE_RELEASED ||
      event->button != BTN_LEFT || state == NULL) {
    return false;
  }
  struct leme_render_hit current = {0};
  if (leme_desk_buttons_allowed(server)) {
    (void)leme_render_at(server, server->cursor->x, server->cursor->y,
                         &current);
  }
  leme_desk_buttons_handle_motion(server, &current);
  const struct leme_desk_target target = state->target;
  const enum leme_render_decoration_part part = state->part;
  const bool matches =
      target.id.serial != 0 &&
      leme_desk_target_equal(leme_desk_buttons_hit(&current), target) &&
      current.decoration_part == part;
  leme_desk_buttons_cancel(server);
  if (matches && leme_desk_buttons_target_available(server, target, part)) {
    leme_desk_target_act(server, target, part);
  }
  return true;
}

void leme_desk_buttons_refresh(struct leme_server *server) {
  if (server->desk_owner.owner != &leme_desk_buttons_owner) {
    return;
  }
  struct leme_render_hit hit = {0};
  if (server->cursor != NULL) {
    (void)leme_render_at(server, server->cursor->x, server->cursor->y, &hit);
  }
  leme_desk_buttons_handle_motion(server, &hit);
}

void leme_desk_buttons_finish(struct leme_server *server) {
  if (server == NULL || server->desk_buttons == NULL) {
    return;
  }
  leme_desk_buttons_cancel(server);
  free(server->desk_buttons);
  server->desk_buttons = NULL;
}

static const struct leme_desk_owner leme_desk_buttons_owner = {
    .button = leme_desk_buttons_release,
    .cancel = leme_desk_buttons_cancel,
};
