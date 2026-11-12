#include "core/command.h"
#include "core/server.h"
#include "input/input.h"
#include "input/internal.h"
#include "protocols/session.h"

#include <libinput.h>
#include <libudev.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <wayland-server-core.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/util/log.h>

#define LEME_SWITCH_LONG_BITS (sizeof(unsigned long) * CHAR_BIT)

struct leme_switch {
  struct leme_server *server;
  struct wlr_input_device *device;
  struct wl_list link;
  struct wl_listener toggle;
  struct wl_listener destroy;
};

static const struct leme_binding *
leme_input_switch_binding(const struct leme_server *server,
                          enum leme_switch_kind kind, bool on) {
  const struct leme_mode *mode = server->active_mode;

  for (size_t i = 0; mode != NULL && i < mode->binding_count; i++) {
    const struct leme_binding *binding = &mode->bindings[i];

    if (binding->trigger == LEME_BINDING_SWITCH &&
        binding->switch_kind == kind && binding->switch_on == on) {
      return binding;
    }
  }
  return NULL;
}

static void leme_input_switch_handle_toggle(struct wl_listener *listener,
                                            void *data) {
  struct leme_switch *toggle = wl_container_of(listener, toggle, toggle);
  const struct wlr_switch_toggle_event *event = data;
  const struct leme_binding *binding;
  enum leme_switch_kind kind;

  if (event->switch_type == WLR_SWITCH_TYPE_LID) {
    kind = LEME_SWITCH_LID;
  } else if (event->switch_type == WLR_SWITCH_TYPE_TABLET_MODE) {
    kind = LEME_SWITCH_TABLET;
  } else {
    return;
  }
  leme_session_notify_activity(toggle->server);
  binding = leme_input_switch_binding(
      toggle->server, kind, event->switch_state == WLR_SWITCH_STATE_ON);
  if (binding != NULL) {
    leme_input_gesture_run(toggle->server, binding);
  }
}

static void leme_input_switch_destroy(struct leme_switch *toggle) {
  wl_list_remove(&toggle->toggle.link);
  wl_list_remove(&toggle->destroy.link);
  wl_list_remove(&toggle->link);
  free(toggle);
}

static void leme_input_switch_handle_destroy(struct wl_listener *listener,
                                             void *data) {
  struct leme_switch *toggle = wl_container_of(listener, toggle, destroy);

  (void)data;
  leme_input_switch_destroy(toggle);
}

void leme_input_switch_add(struct leme_server *server,
                           struct wlr_input_device *device) {
  struct leme_switch *toggle = calloc(1, sizeof(*toggle));

  if (toggle == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to allocate switch state");
    return;
  }
  toggle->server = server;
  toggle->device = device;
  toggle->toggle.notify = leme_input_switch_handle_toggle;
  wl_signal_add(&wlr_switch_from_input_device(device)->events.toggle,
                &toggle->toggle);
  toggle->destroy.notify = leme_input_switch_handle_destroy;
  wl_signal_add(&device->events.destroy, &toggle->destroy);
  wl_list_insert(&server->switches, &toggle->link);
}

void leme_input_switches_finish(struct leme_server *server) {
  struct leme_switch *toggle;
  struct leme_switch *temporary;

  if (server->switch_sync != NULL) {
    wl_event_source_remove(server->switch_sync);
    server->switch_sync = NULL;
  }
  wl_list_for_each_safe(toggle, temporary, &server->switches, link) {
    leme_input_switch_destroy(toggle);
  }
}

static void leme_input_switches_replay(void *data) {
  struct leme_server *server = data;
  const struct leme_switch *toggle;
  bool known[2] = {false, false};
  bool on[2] = {false, false};

  server->switch_sync = NULL;
  wl_list_for_each(toggle, &server->switches, link) {
    unsigned long bits[(SW_CNT + LEME_SWITCH_LONG_BITS - 1) /
                       LEME_SWITCH_LONG_BITS] = {0};

    if (server->session == NULL ||
        !wlr_input_device_is_libinput(toggle->device)) {
      continue;
    }
    struct libinput_device *handle =
        wlr_libinput_get_device_handle(toggle->device);
    struct udev_device *udev = libinput_device_get_udev_device(handle);
    if (udev == NULL) {
      continue;
    }
    const struct wlr_device *device =
        leme_input_session_device(server, udev_device_get_devnum(udev));
    udev_device_unref(udev);
    if (device == NULL || ioctl(device->fd, EVIOCGSW(sizeof(bits)), bits) < 0) {
      continue;
    }
    if (libinput_device_switch_has_switch(handle, LIBINPUT_SWITCH_LID) > 0) {
      known[0] = true;
      on[0] = on[0] || (bits[SW_LID / LEME_SWITCH_LONG_BITS] >>
                            (SW_LID % LEME_SWITCH_LONG_BITS) &
                        1u) != 0;
    }
    if (libinput_device_switch_has_switch(handle, LIBINPUT_SWITCH_TABLET_MODE) >
        0) {
      known[1] = true;
      on[1] = on[1] || (bits[SW_TABLET_MODE / LEME_SWITCH_LONG_BITS] >>
                            (SW_TABLET_MODE % LEME_SWITCH_LONG_BITS) &
                        1u) != 0;
    }
  }
  for (size_t i = 0; i < 2; i++) {
    const struct leme_binding *binding =
        known[i]
            ? leme_input_switch_binding(
                  server, i == 0 ? LEME_SWITCH_LID : LEME_SWITCH_TABLET, on[i])
            : NULL;

    if (binding != NULL &&
        binding->command.type != LEME_COMMAND_RELOAD_CONFIG) {
      leme_input_gesture_run(server, binding);
    }
  }
}

void leme_input_switches_sync(struct leme_server *server) {
  if (server->switch_sync != NULL || wl_list_empty(&server->switches)) {
    return;
  }
  server->switch_sync =
      wl_event_loop_add_idle(wl_display_get_event_loop(server->display),
                             leme_input_switches_replay, server);
}
