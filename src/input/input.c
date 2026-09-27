#include "input/input.h"
#include "input/internal.h"

#include "core/server.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

void leme_input_update_capabilities(struct leme_server *server) {
  uint32_t capabilities = 0;

  if (!wl_list_empty(&server->keyboards)) {
    capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  if (!wl_list_empty(&server->pointers)) {
    capabilities |= WL_SEAT_CAPABILITY_POINTER;
  }
  wlr_seat_set_capabilities(server->seat, capabilities);
}

static void leme_input_handle_new(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, new_input);
  struct wlr_input_device *device = data;

  if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
    leme_input_keyboard_add(server, device);
  } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
    leme_input_pointer_add(server, device);
  }
  leme_input_update_capabilities(server);
}

static void leme_input_handle_new_virtual_keyboard(struct wl_listener *listener,
                                                   void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, new_virtual_keyboard);
  struct wlr_virtual_keyboard_v1 *keyboard = data;

  leme_input_virtual_keyboard_add(server, &keyboard->keyboard);
  leme_input_update_capabilities(server);
}

static void leme_input_handle_new_virtual_pointer(struct wl_listener *listener,
                                                  void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, new_virtual_pointer);
  struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
  struct wlr_input_device *device = &event->new_pointer->pointer.base;

  leme_input_pointer_add(server, device);
  if (event->suggested_output != NULL) {
    wlr_cursor_map_input_to_output(server->cursor, device,
                                   event->suggested_output);
  }
  leme_input_update_capabilities(server);
}

static void leme_input_virtual_init(struct leme_server *server) {
  server->virtual_keyboard_manager =
      wlr_virtual_keyboard_manager_v1_create(server->display);
  server->virtual_pointer_manager =
      wlr_virtual_pointer_manager_v1_create(server->display);
  if (server->virtual_keyboard_manager == NULL ||
      server->virtual_pointer_manager == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to create virtual input protocols");
  }
  if (server->virtual_keyboard_manager != NULL) {
    server->new_virtual_keyboard.notify =
        leme_input_handle_new_virtual_keyboard;
    wl_signal_add(
        &server->virtual_keyboard_manager->events.new_virtual_keyboard,
        &server->new_virtual_keyboard);
  }
  if (server->virtual_pointer_manager != NULL) {
    server->new_virtual_pointer.notify = leme_input_handle_new_virtual_pointer;
    wl_signal_add(&server->virtual_pointer_manager->events.new_virtual_pointer,
                  &server->new_virtual_pointer);
  }
}

static void leme_input_listener_remove(struct wl_listener *listener) {
  if (listener->link.next != NULL) {
    wl_list_remove(&listener->link);
    listener->link.next = NULL;
    listener->link.prev = NULL;
  }
}

void leme_input_init(struct leme_server *server) {
  wl_list_init(&server->keyboards);
  wl_list_init(&server->pointers);
  server->cursor = wlr_cursor_create();
  if (server->cursor == NULL) {
    return;
  }
  wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
  leme_input_pointer_events_init(server);

  server->new_input.notify = leme_input_handle_new;
  wl_signal_add(&server->backend->events.new_input, &server->new_input);
  leme_input_virtual_init(server);
  leme_input_update_capabilities(server);
}

void leme_input_finish(struct leme_server *server) {
  leme_input_listener_remove(&server->new_input);
  leme_input_listener_remove(&server->new_virtual_keyboard);
  leme_input_listener_remove(&server->new_virtual_pointer);
  if (server->cursor == NULL) {
    return;
  }
  leme_input_keyboards_finish(server);
  leme_input_pointers_finish(server);
  wlr_cursor_destroy(server->cursor);
  server->cursor = NULL;
}
