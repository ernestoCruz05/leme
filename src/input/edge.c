#include "core/server.h"
#include "input/edge_tracker.h"
#include "input/input.h"
#include "input/internal.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <limits.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#define LEME_EDGE_SLOTS 16
#define LEME_EDGE_TYPING_MSEC 500u
#define LEME_EDGE_LONG_BITS (sizeof(unsigned long) * CHAR_BIT)

struct leme_edge_reader {
  struct leme_server *server;
  struct wlr_input_device *device;
  dev_t dev;
  int fd;
  struct wl_event_source *source;
  struct input_absinfo x;
  struct input_absinfo y;
  size_t slots;
  bool tool_type;
  int32_t tracked;
  double last_x;
  double last_y;
  uint32_t modifiers;
  uint32_t muted_buttons;
  struct leme_edge_tracker tracker;
};

struct leme_edge_slots {
  uint32_t code;
  int32_t values[LEME_EDGE_SLOTS];
};

const struct wlr_device *
leme_input_session_device(const struct leme_server *server, dev_t dev) {
  const struct wlr_device *device;

  wl_list_for_each(device, &server->session->devices, link) {
    if (device->dev == dev) {
      return device;
    }
  }
  return NULL;
}

static void leme_input_edge_detach(struct leme_edge_reader *reader) {
  if (reader->source != NULL) {
    wl_event_source_remove(reader->source);
    reader->source = NULL;
  }
  if (reader->fd >= 0) {
    close(reader->fd);
    reader->fd = -1;
  }
  reader->tracker.state = LEME_EDGE_IDLE;
  reader->tracked = -1;
}

static bool leme_input_edge_read(const struct leme_edge_reader *reader,
                                 uint32_t code, struct leme_edge_slots *out) {
  out->code = code;
  return ioctl(reader->fd, EVIOCGMTSLOTS(sizeof(*out)), out) >= 0;
}

static bool leme_input_edge_wanted(const struct leme_server *server) {
  const struct leme_mode *mode = server->active_mode;

  for (size_t i = 0; mode != NULL && i < mode->binding_count; i++) {
    if (mode->bindings[i].trigger == LEME_BINDING_EDGE) {
      return true;
    }
  }
  return false;
}

static void leme_input_edge_sync(struct leme_edge_reader *reader) {
  struct leme_server *server = reader->server;
  struct leme_edge_slots ids;
  struct leme_edge_slots xs;
  struct leme_edge_slots ys;
  struct leme_edge_slots tools = {0};
  struct timespec now = {0};

  if (!leme_input_edge_read(reader, ABS_MT_TRACKING_ID, &ids) ||
      !leme_input_edge_read(reader, ABS_MT_POSITION_X, &xs) ||
      !leme_input_edge_read(reader, ABS_MT_POSITION_Y, &ys) ||
      (reader->tool_type &&
       !leme_input_edge_read(reader, ABS_MT_TOOL_TYPE, &tools))) {
    leme_input_edge_detach(reader);
    return;
  }
  struct leme_edge_touch touch = {0};
  int32_t first = -1;
  for (size_t i = 0; i < reader->slots; i++) {
    if (ids.values[i] < 0 || tools.values[i] == MT_TOOL_PALM) {
      continue;
    }
    touch.touches++;
    first = first < 0 ? (int32_t)i : first;
    if (ids.values[i] == reader->tracked) {
      reader->last_x = (double)(xs.values[i] - reader->x.minimum) /
                       (double)reader->x.resolution;
      reader->last_y = (double)(ys.values[i] - reader->y.minimum) /
                       (double)reader->y.resolution;
    }
  }
  if (reader->tracker.state == LEME_EDGE_IDLE && touch.touches > 0) {
    const size_t slot = (size_t)first;
    reader->tracked = ids.values[slot];
    reader->last_x = (double)(xs.values[slot] - reader->x.minimum) /
                     (double)reader->x.resolution;
    reader->last_y = (double)(ys.values[slot] - reader->y.minimum) /
                     (double)reader->y.resolution;
  }
  if (touch.touches == 0) {
    reader->tracked = -1;
  }
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  touch.x = reader->last_x;
  touch.y = reader->last_y;
  touch.msec = (uint32_t)((uint64_t)now.tv_sec * 1000u +
                          (uint64_t)now.tv_nsec / 1000000u);
  uint32_t bound = 0;
  if (touch.touches > 0 && (reader->tracker.state == LEME_EDGE_IDLE ||
                            reader->tracker.state == LEME_EDGE_PENDING)) {
    const struct leme_mode *mode = server->active_mode;
    const bool typing =
        touch.msec - server->typed_msec < LEME_EDGE_TYPING_MSEC &&
        libinput_device_config_dwt_get_enabled(wlr_libinput_get_device_handle(
            reader->device)) == LIBINPUT_CONFIG_DWT_ENABLED;
    const bool blocked =
        typing || leme_input_protocols_shortcuts_inhibited(server) ||
        leme_session_locked(server) || leme_input_pointer_grab_active(server) ||
        server->seat->drag != NULL ||
        server->seat->pointer_state.button_count > 0;

    reader->modifiers = leme_input_modifiers(server);
    for (size_t i = 0; !blocked && mode != NULL && i < mode->binding_count;
         i++) {
      const struct leme_binding *binding = &mode->bindings[i];
      if (binding->trigger == LEME_BINDING_EDGE &&
          binding->modifiers == reader->modifiers) {
        bound |= 1u << binding->edge;
      }
    }
  }
  const enum leme_edge_state before = reader->tracker.state;
  const int steps = leme_edge_tracker_update(
      &reader->tracker, &touch, bound, server->config->gestures.edges.size,
      server->config->gestures.edges.step);
  if (before != LEME_EDGE_CLAIMED &&
      reader->tracker.state == LEME_EDGE_CLAIMED &&
      server->gesture_route.pointer != NULL &&
      &server->gesture_route.pointer->base == reader->device) {
    leme_input_gesture_cancel(server);
  }
  if (steps == 0) {
    return;
  }
  leme_session_notify_activity(server);
  const enum leme_edge_side side = reader->tracker.side;
  const bool vertical = side == LEME_EDGE_LEFT || side == LEME_EDGE_RIGHT;
  const enum leme_gesture_direction direction =
      steps > 0 ? (vertical ? LEME_GESTURE_DOWN : LEME_GESTURE_RIGHT)
                : (vertical ? LEME_GESTURE_UP : LEME_GESTURE_LEFT);
  for (int fired = 0; fired < abs(steps); fired++) {
    const struct leme_binding *binding = leme_input_gesture_binding(
        server, LEME_BINDING_EDGE, 1, reader->modifiers, direction, side);
    if (binding == NULL) {
      return;
    }
    leme_input_gesture_run(server, binding);
  }
}

static int leme_input_edge_readable(int fd, uint32_t mask, void *data) {
  struct leme_edge_reader *reader = data;

  (void)fd;
  if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0 ||
      leme_input_session_device(reader->server, reader->dev) == NULL ||
      !leme_input_edge_wanted(reader->server)) {
    leme_input_edge_detach(reader);
    return 0;
  }
  leme_input_edge_sync(reader);
  return 0;
}

static bool leme_input_edge_attach(struct leme_edge_reader *reader) {
  const struct wlr_device *device =
      leme_input_session_device(reader->server, reader->dev);

  if (device == NULL) {
    return false;
  }
  reader->fd = fcntl(device->fd, F_DUPFD_CLOEXEC, 0);
  if (reader->fd < 0) {
    return false;
  }
  reader->source = wl_event_loop_add_fd(
      wl_display_get_event_loop(reader->server->display), reader->fd,
      WL_EVENT_READABLE, leme_input_edge_readable, reader);
  if (reader->source == NULL) {
    leme_input_edge_detach(reader);
    return false;
  }
  return true;
}

struct leme_edge_reader *
leme_input_edge_reader_create(struct leme_server *server,
                              struct wlr_input_device *device) {
  if (server->session == NULL || !wlr_input_device_is_libinput(device)) {
    return NULL;
  }
  struct libinput_device *handle = wlr_libinput_get_device_handle(device);
  if (libinput_device_config_tap_get_finger_count(handle) == 0) {
    return NULL;
  }
  struct udev_device *udev = libinput_device_get_udev_device(handle);
  if (udev == NULL) {
    return NULL;
  }
  const dev_t dev = udev_device_get_devnum(udev);
  udev_device_unref(udev);

  struct leme_edge_reader *reader = calloc(1, sizeof(*reader));
  if (reader == NULL) {
    return NULL;
  }
  *reader = (struct leme_edge_reader){
      .server = server, .device = device, .dev = dev, .fd = -1, .tracked = -1};
  if (!leme_input_edge_attach(reader)) {
    free(reader);
    return NULL;
  }
  unsigned long
      bits[(ABS_CNT + LEME_EDGE_LONG_BITS - 1) / LEME_EDGE_LONG_BITS] = {0};
  struct input_absinfo slot = {0};
  const bool positions =
      ioctl(reader->fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0 &&
      (bits[ABS_MT_SLOT / LEME_EDGE_LONG_BITS] >>
           (ABS_MT_SLOT % LEME_EDGE_LONG_BITS) &
       1u) != 0 &&
      ioctl(reader->fd, EVIOCGABS(ABS_MT_SLOT), &slot) >= 0 &&
      ioctl(reader->fd, EVIOCGABS(ABS_MT_POSITION_X), &reader->x) >= 0 &&
      ioctl(reader->fd, EVIOCGABS(ABS_MT_POSITION_Y), &reader->y) >= 0 &&
      reader->x.resolution > 0 && reader->y.resolution > 0 &&
      reader->x.maximum > reader->x.minimum &&
      reader->y.maximum > reader->y.minimum && slot.maximum >= 0;
  if (!positions) {
    wlr_log(WLR_INFO,
            "leme: pointer %s reports no finger positions or size, touchpad "
            "edges are off",
            device->name == NULL ? "(unnamed)" : device->name);
    leme_input_edge_reader_destroy(reader);
    return NULL;
  }
  reader->tool_type = (bits[ABS_MT_TOOL_TYPE / LEME_EDGE_LONG_BITS] >>
                           (ABS_MT_TOOL_TYPE % LEME_EDGE_LONG_BITS) &
                       1u) != 0;
  reader->slots = (size_t)slot.maximum + 1 < LEME_EDGE_SLOTS
                      ? (size_t)slot.maximum + 1
                      : LEME_EDGE_SLOTS;
  leme_edge_tracker_init(&reader->tracker,
                         (double)(reader->x.maximum - reader->x.minimum) /
                             (double)reader->x.resolution,
                         (double)(reader->y.maximum - reader->y.minimum) /
                             (double)reader->y.resolution);
  return reader;
}

void leme_input_edge_reader_destroy(struct leme_edge_reader *reader) {
  if (reader == NULL) {
    return;
  }
  leme_input_edge_detach(reader);
  free(reader);
}

bool leme_input_edge_mutes(struct leme_edge_reader *reader,
                           enum leme_edge_event event, uint32_t button) {
  if (reader == NULL) {
    return false;
  }
  if (event != LEME_EDGE_EVENT_RELEASE) {
    if (!leme_input_edge_wanted(reader->server) ||
        (reader->source == NULL && !leme_input_edge_attach(reader))) {
      return false;
    }
    leme_input_edge_sync(reader);
  }
  const enum leme_edge_state state = reader->tracker.state;
  const uint32_t bit =
      button >= BTN_LEFT && button <= BTN_TASK ? 1u << (button - BTN_LEFT) : 0;
  switch (event) {
  case LEME_EDGE_EVENT_MOTION:
  case LEME_EDGE_EVENT_AXIS:
    return state == LEME_EDGE_PENDING || state == LEME_EDGE_CLAIMED;
  case LEME_EDGE_EVENT_GESTURE:
    return state == LEME_EDGE_CLAIMED;
  case LEME_EDGE_EVENT_PRESS:
    if (state != LEME_EDGE_CLAIMED || bit == 0) {
      return false;
    }
    reader->muted_buttons |= bit;
    return true;
  case LEME_EDGE_EVENT_RELEASE:
    if ((reader->muted_buttons & bit) == 0) {
      return false;
    }
    reader->muted_buttons &= ~bit;
    return true;
  }
  return false;
}
