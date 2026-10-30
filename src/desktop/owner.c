#include "desktop/owner.h"

#include "core/server.h"
#include "desktop/buttons.h"
#include "desktop/desktop.h"

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_pointer.h>

static uint32_t leme_desk_owner_bit(uint32_t button) {
  return button >= BTN_MOUSE && button - BTN_MOUSE < 32
             ? 1U << (button - BTN_MOUSE)
             : 0;
}

static void leme_desk_owner_grabbed(struct leme_desk_owner_state *state) {
  struct leme_server *server = wl_container_of(state, server, desk_owner);
  leme_desk_owner_cancel(server);
  leme_desk_unfocus(server);
}

static void leme_desk_owner_handle_pointer_grab(struct wl_listener *listener,
                                                void *data) {
  struct leme_desk_owner_state *state =
      wl_container_of(listener, state, pointer_grab);
  (void)data;
  leme_desk_owner_grabbed(state);
}

static void leme_desk_owner_handle_keyboard_grab(struct wl_listener *listener,
                                                 void *data) {
  struct leme_desk_owner_state *state =
      wl_container_of(listener, state, keyboard_grab);
  (void)data;
  leme_desk_owner_grabbed(state);
}

static void leme_desk_owner_listen(struct leme_server *server) {
  struct leme_desk_owner_state *state = &server->desk_owner;
  if (server->seat == NULL || state->pointer_grab.link.next != NULL) {
    return;
  }
  state->pointer_grab.notify = leme_desk_owner_handle_pointer_grab;
  state->keyboard_grab.notify = leme_desk_owner_handle_keyboard_grab;
  wl_signal_add(&server->seat->events.pointer_grab_begin, &state->pointer_grab);
  wl_signal_add(&server->seat->events.keyboard_grab_begin,
                &state->keyboard_grab);
}

void leme_desk_owner_finish(struct leme_server *server) {
  struct leme_desk_owner_state *state = &server->desk_owner;
  if (state->pointer_grab.link.next != NULL) {
    wl_list_remove(&state->pointer_grab.link);
    wl_list_remove(&state->keyboard_grab.link);
  }
  *state = (struct leme_desk_owner_state){0};
}

void leme_desk_owner_claim(struct leme_server *server,
                           const struct leme_desk_owner *owner,
                           uint32_t button) {
  struct leme_desk_owner_state *state = &server->desk_owner;
  const struct leme_desk_owner *previous = state->owner;
  leme_desk_owner_listen(server);
  state->owner = owner;
  state->buttons |= leme_desk_owner_bit(button);
  if (previous != NULL && previous != owner && previous->cancel != NULL) {
    previous->cancel(server);
  }
}

void leme_desk_owner_swallow(struct leme_server *server, uint32_t button) {
  leme_desk_owner_claim(server, NULL, button);
}

void leme_desk_owner_release(struct leme_server *server,
                             const struct leme_desk_owner *owner) {
  if (server->desk_owner.owner == owner) {
    server->desk_owner.owner = NULL;
  }
}

void leme_desk_owner_handoff(struct leme_server *server,
                             const struct leme_desk_owner *owner,
                             uint32_t button) {
  leme_desk_owner_release(server, owner);
  server->desk_owner.buttons &= ~leme_desk_owner_bit(button);
}

bool leme_desk_owner_modal(const struct leme_server *server) {
  return server->desk_owner.owner != NULL && server->desk_owner.owner->modal;
}

void leme_desk_owner_cancel(struct leme_server *server) {
  const struct leme_desk_owner *previous = server->desk_owner.owner;
  server->desk_owner.owner = NULL;
  if (previous != NULL && previous->cancel != NULL) {
    previous->cancel(server);
  }
  leme_desk_buttons_cancel(server);
}

bool leme_desk_owner_handle_motion(struct leme_server *server) {
  const struct leme_desk_owner *owner = server->desk_owner.owner;
  if (owner == NULL) {
    return false;
  }
  if (owner->motion != NULL) {
    owner->motion(server);
  }
  return owner->modal;
}

bool leme_desk_owner_handle_button(
    struct leme_server *server, const struct wlr_pointer_button_event *event) {
  struct leme_desk_owner_state *state = &server->desk_owner;
  const struct leme_desk_owner *owner = state->owner;
  const uint32_t bit = leme_desk_owner_bit(event->button);
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    const bool held = (state->buttons & bit) != 0;
    state->buttons &= ~bit;
    const bool handled =
        owner != NULL && owner->button != NULL && owner->button(server, event);
    return handled || held;
  }
  if (owner == NULL) {
    return false;
  }
  state->buttons |= bit;
  if (!owner->modal) {
    leme_desk_owner_cancel(server);
  } else if (owner->button != NULL) {
    (void)owner->button(server, event);
  }
  return true;
}

bool leme_desk_owner_handle_axis(struct leme_server *server,
                                 const struct wlr_pointer_axis_event *event) {
  const struct leme_desk_owner *owner = server->desk_owner.owner;
  if (owner == NULL || !owner->modal) {
    return false;
  }
  if (owner->axis != NULL) {
    owner->axis(server, event);
  }
  return true;
}

bool leme_desk_owner_handle_key(struct leme_server *server, xkb_keysym_t keysym,
                                uint32_t modifiers, const char *utf8,
                                size_t length, bool reserved) {
  const struct leme_desk_owner *owner = server->desk_owner.owner;
  if (owner == NULL || !owner->modal || owner->key == NULL) {
    return false;
  }
  if (reserved) {
    leme_desk_owner_cancel(server);
    return false;
  }
  return owner->key(server, keysym, modifiers, utf8, length);
}
