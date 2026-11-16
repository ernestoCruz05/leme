#include "core/server.h"
#include "input/internal.h"
#include "shell/view.h"

#include <fnmatch.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>

static void leme_input_pass_end(struct leme_server *server) {
  wl_list_remove(&server->pass_surface_destroy.link);
  server->pass_surface = NULL;
  server->pass_count = 0;
}

static void leme_input_pass_handle_surface_destroy(struct wl_listener *listener,
                                                   void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, pass_surface_destroy);

  (void)data;
  leme_input_pass_end(server);
}

static void leme_input_pass_send(struct leme_server *server,
                                 const struct wlr_keyboard_modifiers *entering,
                                 uint32_t time_msec, uint32_t keycode,
                                 enum wl_keyboard_key_state state, bool leave) {
  struct wl_resource *surface = server->pass_surface->resource;
  struct wlr_seat_client *client = wlr_seat_client_for_wl_client(
      server->seat, wl_resource_get_client(surface));
  struct wl_resource *resource;

  if (client == NULL) {
    return;
  }
  wl_resource_for_each(resource, &client->keyboards) {
    if (entering != NULL) {
      struct wl_array keys;

      wl_array_init(&keys);
      wl_keyboard_send_enter(resource, wlr_seat_client_next_serial(client),
                             surface, &keys);
      wl_keyboard_send_modifiers(resource, wlr_seat_client_next_serial(client),
                                 entering->depressed, entering->latched,
                                 entering->locked, entering->group);
    }
    wl_keyboard_send_key(resource, wlr_seat_client_next_serial(client),
                         time_msec, keycode, state);
    if (leave && client != server->seat->keyboard_state.focused_client) {
      wl_keyboard_send_leave(resource, wlr_seat_client_next_serial(client),
                             surface);
    }
  }
}

bool leme_input_pass_press(struct leme_server *server,
                           struct wlr_keyboard *keyboard, const char *pattern,
                           uint32_t time_msec, uint32_t keycode) {
  struct wlr_surface *surface = NULL;
  const struct wlr_seat_client *client;
  struct leme_view *view;

  wl_list_for_each(view, &server->views, link) {
    const char *identity = leme_view_identity(view);

    if (view->mapped && identity != NULL &&
        fnmatch(pattern, identity, 0) == 0) {
      surface = leme_view_surface(view);
      break;
    }
  }
  if (surface == NULL ||
      server->pass_count == LEME_ARRAY_LENGTH(server->pass_keycodes)) {
    return false;
  }
  client = wlr_seat_client_for_wl_client(
      server->seat, wl_resource_get_client(surface->resource));
  if (client == NULL || client == server->seat->keyboard_state.focused_client) {
    return false;
  }
  const bool entering = server->pass_surface == NULL;
  if (entering) {
    server->pass_surface = surface;
    server->pass_surface_destroy.notify =
        leme_input_pass_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &server->pass_surface_destroy);
  } else if (wl_resource_get_client(server->pass_surface->resource) !=
             client->client) {
    return false;
  }
  server->pass_keyboards[server->pass_count] = keyboard;
  server->pass_keycodes[server->pass_count] = keycode;
  server->pass_count++;
  wlr_seat_set_keyboard(server->seat, keyboard);
  leme_input_pass_send(server, entering ? &keyboard->modifiers : NULL,
                       time_msec, keycode, WL_KEYBOARD_KEY_STATE_PRESSED,
                       false);
  return true;
}

void leme_input_pass_release(struct leme_server *server,
                             const struct wlr_keyboard *keyboard,
                             uint32_t time_msec, uint32_t keycode) {
  size_t index;

  for (index = 0; index < server->pass_count; index++) {
    if (server->pass_keyboards[index] == keyboard &&
        server->pass_keycodes[index] == keycode) {
      break;
    }
  }
  if (index == server->pass_count) {
    return;
  }
  server->pass_count--;
  server->pass_keyboards[index] = server->pass_keyboards[server->pass_count];
  server->pass_keycodes[index] = server->pass_keycodes[server->pass_count];
  leme_input_pass_send(server, NULL, time_msec, keycode,
                       WL_KEYBOARD_KEY_STATE_RELEASED, server->pass_count == 0);
  if (server->pass_count == 0) {
    leme_input_pass_end(server);
  }
}

void leme_input_pass_keyboard_destroyed(struct leme_server *server,
                                        const struct wlr_keyboard *keyboard) {
  struct timespec now;
  size_t index = 0;

  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  while (index < server->pass_count) {
    if (server->pass_keyboards[index] != keyboard) {
      index++;
      continue;
    }
    leme_input_pass_release(
        server, keyboard, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000),
        server->pass_keycodes[index]);
  }
}
