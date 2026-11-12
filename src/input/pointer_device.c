#include "input/pointer_internal.h"
#include "input/input.h"
#include "input/internal.h"
#include "input/public.h"
#include "public/server.h"

#include "config/config.h"
#include "config/live.h"
#include "core/server.h"
#include "protocols/session.h"

#include <inttypes.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/util/log.h>

struct leme_pointer_settings
leme_input_pointer_settings(const struct leme_config *config,
                            const char *device_name) {
  struct leme_pointer_settings settings = config->pointer_defaults;
  size_t index;

  for (index = 0; device_name != NULL && index < config->pointer_rule_count;
       index++) {
    const struct leme_pointer_rule *rule = &config->pointer_rules[index];
    const struct leme_pointer_settings *override = &rule->settings;

    if (strcmp(rule->name, device_name) != 0) {
      continue;
    }
    if ((override->fields & LEME_POINTER_PROFILE) != 0) {
      settings.profile = override->profile;
    }
    if ((override->fields & LEME_POINTER_SPEED) != 0) {
      settings.speed = override->speed;
    }
    if ((override->fields & LEME_POINTER_NATURAL_SCROLL) != 0) {
      settings.natural_scroll = override->natural_scroll;
    }
    if ((override->fields & LEME_POINTER_LEFT_HANDED) != 0) {
      settings.left_handed = override->left_handed;
    }
    if ((override->fields & LEME_POINTER_TAP) != 0) {
      settings.tap = override->tap;
    }
    if ((override->fields & LEME_POINTER_DISABLE_WHILE_TYPING) != 0) {
      settings.disable_while_typing = override->disable_while_typing;
    }
    if ((override->fields & LEME_POINTER_CLICK_METHOD) != 0) {
      settings.click_method = override->click_method;
    }
    if ((override->fields & LEME_POINTER_SCROLL_METHOD) != 0) {
      settings.scroll_method = override->scroll_method;
    }
    if ((override->fields & LEME_POINTER_SCROLL_BUTTON) != 0) {
      settings.scroll_button = override->scroll_button;
    }
    if ((override->fields & LEME_POINTER_MIDDLE_EMULATION) != 0) {
      settings.middle_emulation = override->middle_emulation;
    }
    if ((override->fields & LEME_POINTER_TAP_BUTTON_MAP) != 0) {
      settings.tap_button_map = override->tap_button_map;
    }
    if ((override->fields & LEME_POINTER_TAP_DRAG) != 0) {
      settings.tap_drag = override->tap_drag;
    }
    if ((override->fields & LEME_POINTER_DRAG_LOCK) != 0) {
      settings.drag_lock = override->drag_lock;
    }
    if ((override->fields & LEME_POINTER_DRAG_FINGERS) != 0) {
      settings.drag_fingers = override->drag_fingers;
    }
    if ((override->fields & LEME_POINTER_SCROLL_FACTOR) != 0) {
      settings.scroll_factor = override->scroll_factor;
    }
    settings.fields |= override->fields;
    break;
  }
  return settings;
}

static void leme_input_pointer_status(const struct wlr_input_device *device,
                                      const char *property,
                                      enum libinput_config_status status) {
  if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
    wlr_log(WLR_ERROR, "leme: pointer %s failed to set %s: %s",
            device->name == NULL ? "(unnamed)" : device->name, property,
            libinput_config_status_to_str(status));
  }
}

static enum libinput_config_click_method
leme_input_click_method(enum leme_pointer_click_method method) {
  switch (method) {
  case LEME_POINTER_CLICK_NONE:
    return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
  case LEME_POINTER_CLICK_BUTTON_AREAS:
    return LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
  case LEME_POINTER_CLICK_CLICKFINGER:
    return LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
  }
  return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}

static enum libinput_config_scroll_method
leme_input_scroll_method(enum leme_pointer_scroll_method method) {
  switch (method) {
  case LEME_POINTER_SCROLL_NONE:
    return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
  case LEME_POINTER_SCROLL_TWO_FINGER:
    return LIBINPUT_CONFIG_SCROLL_2FG;
  case LEME_POINTER_SCROLL_EDGE:
    return LIBINPUT_CONFIG_SCROLL_EDGE;
  case LEME_POINTER_SCROLL_ON_BUTTON_DOWN:
    return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
  }
  return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}

static enum libinput_config_tap_button_map
leme_input_tap_button_map(enum leme_pointer_tap_button_map map) {
  return map == LEME_POINTER_TAP_MAP_LMR ? LIBINPUT_CONFIG_TAP_MAP_LMR
                                         : LIBINPUT_CONFIG_TAP_MAP_LRM;
}

static enum libinput_config_drag_lock_state
leme_input_drag_lock(enum leme_pointer_drag_lock lock) {
  switch (lock) {
  case LEME_POINTER_DRAG_LOCK_NONE:
    return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
  case LEME_POINTER_DRAG_LOCK_TIMEOUT:
    return LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT;
  case LEME_POINTER_DRAG_LOCK_STICKY:
    return LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY;
  }
  return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
}

static enum libinput_config_3fg_drag_state
leme_input_drag_fingers(uint32_t fingers) {
  switch (fingers) {
  case 3:
    return LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG;
  case 4:
    return LIBINPUT_CONFIG_3FG_DRAG_ENABLED_4FG;
  default:
    return LIBINPUT_CONFIG_3FG_DRAG_DISABLED;
  }
}

static uint32_t
leme_input_drag_fingers_of(enum libinput_config_3fg_drag_state state) {
  switch (state) {
  case LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG:
    return 3;
  case LIBINPUT_CONFIG_3FG_DRAG_ENABLED_4FG:
    return 4;
  default:
    return 0;
  }
}

static void
leme_input_pointer_read_options(struct libinput_device *dev,
                                struct leme_input_target_info *info) {
  info->has_disable_while_typing =
      libinput_device_config_dwt_is_available(dev) != 0;
  if (info->has_disable_while_typing) {
    info->current_disable_while_typing =
        libinput_device_config_dwt_get_enabled(dev) ==
        LIBINPUT_CONFIG_DWT_ENABLED;
  }
  info->click_methods = libinput_device_config_click_get_methods(dev);
  switch (libinput_device_config_click_get_method(dev)) {
  case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
    info->current_click_method = LEME_POINTER_CLICK_BUTTON_AREAS;
    break;
  case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
    info->current_click_method = LEME_POINTER_CLICK_CLICKFINGER;
    break;
  default:
    info->current_click_method = LEME_POINTER_CLICK_NONE;
    break;
  }
  info->scroll_methods = libinput_device_config_scroll_get_methods(dev);
  switch (libinput_device_config_scroll_get_method(dev)) {
  case LIBINPUT_CONFIG_SCROLL_2FG:
    info->current_scroll_method = LEME_POINTER_SCROLL_TWO_FINGER;
    break;
  case LIBINPUT_CONFIG_SCROLL_EDGE:
    info->current_scroll_method = LEME_POINTER_SCROLL_EDGE;
    break;
  case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
    info->current_scroll_method = LEME_POINTER_SCROLL_ON_BUTTON_DOWN;
    break;
  default:
    info->current_scroll_method = LEME_POINTER_SCROLL_NONE;
    break;
  }
  info->has_scroll_button =
      (info->scroll_methods & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) != 0;
  if (info->has_scroll_button) {
    const uint32_t button = libinput_device_config_scroll_get_button(dev);
    info->has_current_scroll_button = button >= BTN_LEFT && button <= BTN_TASK;
    if (info->has_current_scroll_button) {
      info->current_scroll_button = button - BTN_LEFT;
    }
  }
  info->has_middle_emulation =
      libinput_device_config_middle_emulation_is_available(dev) != 0;
  if (info->has_middle_emulation) {
    info->current_middle_emulation =
        libinput_device_config_middle_emulation_get_enabled(dev) ==
        LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED;
  }
  if (libinput_device_config_tap_get_finger_count(dev) > 0) {
    info->current_tap_button_map = libinput_device_config_tap_get_button_map(
                                       dev) == LIBINPUT_CONFIG_TAP_MAP_LMR
                                       ? LEME_POINTER_TAP_MAP_LMR
                                       : LEME_POINTER_TAP_MAP_LRM;
    info->current_tap_drag = libinput_device_config_tap_get_drag_enabled(dev) ==
                             LIBINPUT_CONFIG_DRAG_ENABLED;
    switch (libinput_device_config_tap_get_drag_lock_enabled(dev)) {
    case LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT:
      info->current_drag_lock = LEME_POINTER_DRAG_LOCK_TIMEOUT;
      break;
    case LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY:
      info->current_drag_lock = LEME_POINTER_DRAG_LOCK_STICKY;
      break;
    default:
      info->current_drag_lock = LEME_POINTER_DRAG_LOCK_NONE;
      break;
    }
  }
  const int drag_finger_count =
      libinput_device_config_3fg_drag_get_finger_count(dev);
  info->drag_finger_count =
      drag_finger_count > 0 ? (uint32_t)drag_finger_count : 0;
  if (info->drag_finger_count >= 3) {
    info->current_drag_fingers = leme_input_drag_fingers_of(
        libinput_device_config_3fg_drag_get_enabled(dev));
  }
}

static void leme_input_configure_pointer(struct leme_server *server,
                                         struct leme_pointer *pointer,
                                         const struct leme_config *config) {
  struct wlr_input_device *device = pointer->device;
  struct leme_pointer_settings settings;
  struct libinput_device *libinput_device;
  enum libinput_config_accel_profile profile;
  uint32_t profiles;

  if (config == NULL) {
    return;
  }
  settings = leme_input_pointer_settings(config, device->name);
  if (server != NULL && server->config_store != NULL) {
    size_t count = 0;
    const struct leme_scoped_override *ovs =
        leme_config_store_overrides(server->config_store, &count);
    for (size_t i = 0; i < count; i++) {
      if (!ovs[i].has_target || ovs[i].target.kind != LEME_PUBLIC_INPUT ||
          ovs[i].target.id.serial != pointer->public_id.serial ||
          ovs[i].path_count != 1 || ovs[i].path == NULL ||
          ovs[i].path[0] == NULL) {
        continue;
      }
      const char *key = ovs[i].path[0];
      const struct leme_public_value *value = ovs[i].value;
      struct leme_public_text text = {0};
      bool flag = false;
      double number = 0.0;
      unsigned choice = 0;
      const bool is_text = leme_public_as_text(value, &text) == LEME_PUBLIC_OK;
      const bool is_bool = leme_public_as_bool(value, &flag) == LEME_PUBLIC_OK;
      const bool is_number =
          leme_public_as_number(value, &number) == LEME_PUBLIC_OK;

      if (strcmp(key, "accel_profile") == 0 && is_text) {
        if (text.length == 4 && memcmp(text.data, "flat", 4) == 0) {
          settings.profile = LEME_POINTER_ACCEL_FLAT;
        } else if (text.length == 8 && memcmp(text.data, "adaptive", 8) == 0) {
          settings.profile = LEME_POINTER_ACCEL_ADAPTIVE;
        }
      } else if (strcmp(key, "accel_speed") == 0 && is_number) {
        settings.speed = number;
      } else if (strcmp(key, "natural_scroll") == 0 && is_bool) {
        settings.natural_scroll = flag;
      } else if (strcmp(key, "left_handed") == 0 && is_bool) {
        settings.left_handed = flag;
      } else if (strcmp(key, "tap") == 0 && is_bool) {
        settings.tap = flag;
      } else if (strcmp(key, "disable_while_typing") == 0 && is_bool) {
        settings.disable_while_typing = flag;
        settings.fields |= LEME_POINTER_DISABLE_WHILE_TYPING;
      } else if (strcmp(key, "middle_emulation") == 0 && is_bool) {
        settings.middle_emulation = flag;
        settings.fields |= LEME_POINTER_MIDDLE_EMULATION;
      } else if (strcmp(key, "tap_drag") == 0 && is_bool) {
        settings.tap_drag = flag;
        settings.fields |= LEME_POINTER_TAP_DRAG;
      } else if (strcmp(key, "click_method") == 0 && is_text &&
                 leme_pointer_choice_find(LEME_POINTER_CHOICE_CLICK_METHOD,
                                          text.data, text.length, &choice)) {
        settings.click_method = choice;
        settings.fields |= LEME_POINTER_CLICK_METHOD;
      } else if (strcmp(key, "scroll_method") == 0 && is_text &&
                 leme_pointer_choice_find(LEME_POINTER_CHOICE_SCROLL_METHOD,
                                          text.data, text.length, &choice)) {
        settings.scroll_method = choice;
        settings.fields |= LEME_POINTER_SCROLL_METHOD;
      } else if (strcmp(key, "scroll_button") == 0 && is_text &&
                 leme_pointer_choice_find(LEME_POINTER_CHOICE_SCROLL_BUTTON,
                                          text.data, text.length, &choice)) {
        settings.scroll_button = choice;
        settings.fields |= LEME_POINTER_SCROLL_BUTTON;
      } else if (strcmp(key, "tap_button_map") == 0 && is_text &&
                 leme_pointer_choice_find(LEME_POINTER_CHOICE_TAP_BUTTON_MAP,
                                          text.data, text.length, &choice)) {
        settings.tap_button_map = choice;
        settings.fields |= LEME_POINTER_TAP_BUTTON_MAP;
      } else if (strcmp(key, "drag_lock") == 0 && is_text &&
                 leme_pointer_choice_find(LEME_POINTER_CHOICE_DRAG_LOCK,
                                          text.data, text.length, &choice)) {
        settings.drag_lock = choice;
        settings.fields |= LEME_POINTER_DRAG_LOCK;
      } else if (strcmp(key, "drag_fingers") == 0 && is_number) {
        settings.drag_fingers = number == 3.0 ? 3 : number == 4.0 ? 4 : 0;
        settings.fields |= LEME_POINTER_DRAG_FINGERS;
      } else if (strcmp(key, "scroll_factor") == 0 && is_number) {
        settings.scroll_factor = number;
        settings.fields |= LEME_POINTER_SCROLL_FACTOR;
      }
    }
  }
  pointer->scroll_factor = (settings.fields & LEME_POINTER_SCROLL_FACTOR) != 0
                               ? settings.scroll_factor
                               : 1.0;
  if (!wlr_input_device_is_libinput(device)) {
    return;
  }
  libinput_device = wlr_libinput_get_device_handle(device);
  if (libinput_device == NULL) {
    return;
  }
  profile = settings.profile == LEME_POINTER_ACCEL_FLAT
                ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
  profiles = libinput_device_config_accel_get_profiles(libinput_device);
  if ((profiles & profile) != 0) {
    leme_input_pointer_status(
        device, "accel_profile",
        libinput_device_config_accel_set_profile(libinput_device, profile));
  }
  if (libinput_device_config_accel_is_available(libinput_device)) {
    leme_input_pointer_status(device, "accel_speed",
                              libinput_device_config_accel_set_speed(
                                  libinput_device, settings.speed));
  }
  if (libinput_device_config_scroll_has_natural_scroll(libinput_device)) {
    leme_input_pointer_status(
        device, "natural_scroll",
        libinput_device_config_scroll_set_natural_scroll_enabled(
            libinput_device, settings.natural_scroll));
  }
  if (libinput_device_config_left_handed_is_available(libinput_device)) {
    leme_input_pointer_status(device, "left_handed",
                              libinput_device_config_left_handed_set(
                                  libinput_device, settings.left_handed));
  }
  if (libinput_device_config_tap_get_finger_count(libinput_device) > 0) {
    leme_input_pointer_status(
        device, "tap",
        libinput_device_config_tap_set_enabled(
            libinput_device, settings.tap ? LIBINPUT_CONFIG_TAP_ENABLED
                                          : LIBINPUT_CONFIG_TAP_DISABLED));
  }
  if (libinput_device_config_dwt_is_available(libinput_device)) {
    const enum libinput_config_dwt_state dwt =
        (settings.fields & LEME_POINTER_DISABLE_WHILE_TYPING) == 0
            ? libinput_device_config_dwt_get_default_enabled(libinput_device)
        : settings.disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED
                                        : LIBINPUT_CONFIG_DWT_DISABLED;
    leme_input_pointer_status(
        device, "disable_while_typing",
        libinput_device_config_dwt_set_enabled(libinput_device, dwt));
  }
  const uint32_t click_methods =
      libinput_device_config_click_get_methods(libinput_device);
  if (click_methods != 0) {
    const enum libinput_config_click_method method =
        (settings.fields & LEME_POINTER_CLICK_METHOD) == 0
            ? libinput_device_config_click_get_default_method(libinput_device)
            : leme_input_click_method(settings.click_method);
    if (method == LIBINPUT_CONFIG_CLICK_METHOD_NONE ||
        (click_methods & method) != 0) {
      leme_input_pointer_status(
          device, "click_method",
          libinput_device_config_click_set_method(libinput_device, method));
    }
  }
  const uint32_t scroll_methods =
      libinput_device_config_scroll_get_methods(libinput_device);
  if (scroll_methods != LIBINPUT_CONFIG_SCROLL_NO_SCROLL) {
    const enum libinput_config_scroll_method method =
        (settings.fields & LEME_POINTER_SCROLL_METHOD) == 0
            ? libinput_device_config_scroll_get_default_method(libinput_device)
            : leme_input_scroll_method(settings.scroll_method);
    if (method == LIBINPUT_CONFIG_SCROLL_NO_SCROLL ||
        (scroll_methods & method) != 0) {
      leme_input_pointer_status(
          device, "scroll_method",
          libinput_device_config_scroll_set_method(libinput_device, method));
    }
  }
  if ((scroll_methods & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) != 0) {
    const uint32_t button =
        (settings.fields & LEME_POINTER_SCROLL_BUTTON) == 0
            ? libinput_device_config_scroll_get_default_button(libinput_device)
            : BTN_LEFT + settings.scroll_button;
    if (button == 0 ||
        libinput_device_pointer_has_button(libinput_device, button) == 1) {
      leme_input_pointer_status(
          device, "scroll_button",
          libinput_device_config_scroll_set_button(libinput_device, button));
    }
  }
  if (libinput_device_config_middle_emulation_is_available(libinput_device)) {
    const enum libinput_config_middle_emulation_state middle =
        (settings.fields & LEME_POINTER_MIDDLE_EMULATION) == 0
            ? libinput_device_config_middle_emulation_get_default_enabled(
                  libinput_device)
        : settings.middle_emulation ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                                    : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
    leme_input_pointer_status(
        device, "middle_emulation",
        libinput_device_config_middle_emulation_set_enabled(libinput_device,
                                                            middle));
  }
  if (libinput_device_config_tap_get_finger_count(libinput_device) > 0) {
    const enum libinput_config_tap_button_map map =
        (settings.fields & LEME_POINTER_TAP_BUTTON_MAP) == 0
            ? libinput_device_config_tap_get_default_button_map(libinput_device)
            : leme_input_tap_button_map(settings.tap_button_map);
    const enum libinput_config_drag_state drag =
        (settings.fields & LEME_POINTER_TAP_DRAG) == 0
            ? libinput_device_config_tap_get_default_drag_enabled(
                  libinput_device)
        : settings.tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED
                            : LIBINPUT_CONFIG_DRAG_DISABLED;
    const enum libinput_config_drag_lock_state lock =
        (settings.fields & LEME_POINTER_DRAG_LOCK) == 0
            ? libinput_device_config_tap_get_default_drag_lock_enabled(
                  libinput_device)
            : leme_input_drag_lock(settings.drag_lock);
    leme_input_pointer_status(
        device, "tap_button_map",
        libinput_device_config_tap_set_button_map(libinput_device, map));
    leme_input_pointer_status(
        device, "tap_drag",
        libinput_device_config_tap_set_drag_enabled(libinput_device, drag));
    leme_input_pointer_status(device, "drag_lock",
                              libinput_device_config_tap_set_drag_lock_enabled(
                                  libinput_device, lock));
  }
  const int drag_finger_count =
      libinput_device_config_3fg_drag_get_finger_count(libinput_device);
  if (drag_finger_count >= 3) {
    uint32_t fingers =
        (settings.fields & LEME_POINTER_DRAG_FINGERS) == 0
            ? leme_input_drag_fingers_of(
                  libinput_device_config_3fg_drag_get_default_enabled(
                      libinput_device))
            : settings.drag_fingers;
    if (fingers != 0 &&
        (leme_config_swipe_fingers(config) & (1u << fingers)) != 0) {
      wlr_log(WLR_INFO,
              "leme: pointer %s keeps finger drags off, %" PRIu32
              "-finger swipes switch tags or are bound",
              device->name == NULL ? "(unnamed)" : device->name, fingers);
      fingers = 0;
    }
    if (fingers <= (uint32_t)drag_finger_count) {
      leme_input_pointer_status(
          device, "drag_fingers",
          libinput_device_config_3fg_drag_set_enabled(
              libinput_device, leme_input_drag_fingers(fingers)));
    }
  }
  if (libinput_device_config_scroll_get_method(libinput_device) ==
      LIBINPUT_CONFIG_SCROLL_EDGE) {
    bool clash = false;
    for (size_t i = 0; !clash && i < config->mode_count; i++) {
      for (size_t j = 0; !clash && j < config->modes[i].binding_count; j++) {
        const struct leme_binding *binding = &config->modes[i].bindings[j];
        clash = binding->trigger == LEME_BINDING_EDGE &&
                (binding->edge == LEME_EDGE_RIGHT ||
                 binding->edge == LEME_EDGE_BOTTOM);
      }
    }
    if (clash) {
      wlr_log(WLR_INFO,
              "leme: pointer %s edge scrolling loses touches to right and "
              "bottom edge bindings",
              device->name == NULL ? "(unnamed)" : device->name);
    }
  }
  wlr_log(WLR_INFO, "leme: configured pointer %s",
          device->name == NULL ? "(unnamed)" : device->name);
}

void leme_input_apply_pointer_config(struct leme_server *server,
                                     const struct leme_config *config) {
  struct leme_pointer *pointer;

  if (server == NULL || config == NULL || server->cursor == NULL) {
    return;
  }
  leme_input_workspace_gesture_cancel(server);
  wl_list_for_each(pointer, &server->pointers, link) {
    leme_input_configure_pointer(server, pointer, config);
  }
  leme_public_server_invalidate(server);
}

static void leme_input_handle_pointer_destroy(struct wl_listener *listener,
                                              void *data) {
  struct leme_pointer *pointer = wl_container_of(listener, pointer, destroy);
  struct leme_server *server = pointer->server;

  (void)data;
  if (server->gesture_route.pointer != NULL &&
      &server->gesture_route.pointer->base == pointer->device) {
    leme_input_gesture_cancel(server);
  }
  wlr_cursor_detach_input_device(server->cursor, pointer->device);
  leme_input_edge_reader_destroy(pointer->edge);
  wl_list_remove(&pointer->destroy.link);
  wl_list_remove(&pointer->link);
  if (server->config_store != NULL) {
    struct leme_control_target target = {
        .kind = LEME_PUBLIC_INPUT,
        .id = pointer->public_id,
    };
    leme_config_store_drop_target(server->config_store, &target);
  }
  free(pointer);
  leme_input_update_capabilities(server);
  leme_public_server_invalidate(server);
}

void leme_input_pointer_add(struct leme_server *server,
                            struct wlr_input_device *device) {
  struct leme_pointer *pointer = calloc(1, sizeof(*pointer));

  if (pointer == NULL) {
    return;
  }
  pointer->server = server;
  pointer->device = device;
  if (leme_public_model_available(server->public_model) &&
      leme_public_model_issue_id(server->public_model, &pointer->public_id) !=
          LEME_PUBLIC_OK) {
    leme_public_model_disable(server->public_model);
  }
  pointer->scroll_factor = 1.0;
  leme_input_configure_pointer(server, pointer, server->config);
  pointer->edge = leme_input_edge_reader_create(server, device);
  pointer->destroy.notify = leme_input_handle_pointer_destroy;
  wl_signal_add(&device->events.destroy, &pointer->destroy);
  wl_list_insert(&server->pointers, &pointer->link);
  wlr_cursor_attach_input_device(server->cursor, device);
  leme_public_server_invalidate(server);
}

enum leme_public_status
leme_input_public_pointers(const struct leme_server *server,
                           leme_public_input_visitor visit, void *context) {
  if (server == NULL || visit == NULL || server->pointers.next == NULL)
    return LEME_PUBLIC_INVALID;
  if (leme_session_locked(server))
    return LEME_PUBLIC_LOCKED;
  if (!leme_public_model_available(server->public_model))
    return LEME_PUBLIC_UNAVAILABLE;
  const struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    struct leme_public_input_info info = {
        .id = pointer->public_id,
        .name = pointer->device->name,
        .seat = server->seat == NULL ? NULL : server->seat->name,
        .libinput = wlr_input_device_is_libinput(pointer->device)};
    struct libinput_device *device =
        info.libinput ? wlr_libinput_get_device_handle(pointer->device) : NULL;
    if (device != NULL) {
      info.has_ids = true;
      info.vendor = libinput_device_get_id_vendor(device);
      info.product = libinput_device_get_id_product(device);
      const uint32_t profiles =
          libinput_device_config_accel_get_profiles(device);
      info.adaptive = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE) != 0;
      info.flat = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) != 0;
      const enum libinput_config_accel_profile profile =
          libinput_device_config_accel_get_profile(device);
      if (profile == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE)
        info.accel_profile = "adaptive";
      if (profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT)
        info.accel_profile = "flat";
      info.has_accel = libinput_device_config_accel_is_available(device) != 0;
      if (info.has_accel)
        info.accel_speed = libinput_device_config_accel_get_speed(device);
      info.has_natural_scroll =
          libinput_device_config_scroll_has_natural_scroll(device) != 0;
      if (info.has_natural_scroll)
        info.natural_scroll =
            libinput_device_config_scroll_get_natural_scroll_enabled(device) !=
            0;
      info.has_left_handed =
          libinput_device_config_left_handed_is_available(device) != 0;
      if (info.has_left_handed)
        info.left_handed = libinput_device_config_left_handed_get(device) != 0;
      info.has_tap = libinput_device_config_tap_get_finger_count(device) > 0;
      if (info.has_tap)
        info.tap = libinput_device_config_tap_get_enabled(device) ==
                   LIBINPUT_CONFIG_TAP_ENABLED;
      struct leme_input_target_info options = {0};
      leme_input_pointer_read_options(device, &options);
      info.has_disable_while_typing = options.has_disable_while_typing;
      info.disable_while_typing = options.current_disable_while_typing;
      if (options.click_methods != 0)
        info.click_method = leme_pointer_choice_name(
            LEME_POINTER_CHOICE_CLICK_METHOD, options.current_click_method);
      if (options.scroll_methods != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
        info.scroll_method = leme_pointer_choice_name(
            LEME_POINTER_CHOICE_SCROLL_METHOD, options.current_scroll_method);
      info.has_scroll_button = options.has_scroll_button;
      if (options.has_current_scroll_button)
        info.scroll_button = leme_pointer_choice_name(
            LEME_POINTER_CHOICE_SCROLL_BUTTON, options.current_scroll_button);
      info.has_middle_emulation = options.has_middle_emulation;
      info.middle_emulation = options.current_middle_emulation;
      if (info.has_tap) {
        info.tap_button_map = leme_pointer_choice_name(
            LEME_POINTER_CHOICE_TAP_BUTTON_MAP, options.current_tap_button_map);
        info.tap_drag = options.current_tap_drag;
        info.drag_lock = leme_pointer_choice_name(LEME_POINTER_CHOICE_DRAG_LOCK,
                                                  options.current_drag_lock);
      }
      info.has_drag_fingers = options.drag_finger_count >= 3;
      info.drag_fingers = options.current_drag_fingers;
    }
    info.scroll_factor = pointer->scroll_factor;
    const enum leme_public_status status = visit(context, &info);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  return LEME_PUBLIC_OK;
}

static bool leme_input_pointer_resolve_id(const struct leme_server *server,
                                          struct leme_public_id id,
                                          struct leme_input_target_info *info) {
  if (server == NULL || info == NULL || server->pointers.next == NULL) {
    return false;
  }
  const struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    if (pointer->public_id.serial == id.serial) {
      info->exists = true;
      info->is_keyboard = false;
      info->is_libinput = wlr_input_device_is_libinput(pointer->device);
      info->current_scroll_factor = pointer->scroll_factor;
      if (info->is_libinput) {
        struct libinput_device *dev =
            wlr_libinput_get_device_handle(pointer->device);
        if (dev != NULL) {
          const uint32_t profiles =
              libinput_device_config_accel_get_profiles(dev);
          info->adaptive =
              (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE) != 0;
          info->flat = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) != 0;
          info->current_accel_profile =
              libinput_device_config_accel_get_profile(dev);
          info->has_accel = libinput_device_config_accel_is_available(dev) != 0;
          if (info->has_accel) {
            info->current_accel_speed =
                libinput_device_config_accel_get_speed(dev);
          }
          info->has_natural_scroll =
              libinput_device_config_scroll_has_natural_scroll(dev) != 0;
          if (info->has_natural_scroll) {
            info->current_natural_scroll =
                libinput_device_config_scroll_get_natural_scroll_enabled(dev) !=
                0;
          }
          info->has_left_handed =
              libinput_device_config_left_handed_is_available(dev) != 0;
          if (info->has_left_handed) {
            info->current_left_handed =
                libinput_device_config_left_handed_get(dev) != 0;
          }
          info->has_tap = libinput_device_config_tap_get_finger_count(dev) > 0;
          if (info->has_tap) {
            info->current_tap = libinput_device_config_tap_get_enabled(dev) ==
                                LIBINPUT_CONFIG_TAP_ENABLED;
          }
          leme_input_pointer_read_options(dev, info);
        }
      }
      return true;
    }
  }
  return false;
}

enum libinput_config_status
leme_input_apply_pointer_setting(struct leme_server *server,
                                 struct leme_public_id id,
                                 const struct leme_input_setting_val *val) {
  if (server == NULL || val == NULL || server->pointers.next == NULL) {
    return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
  }
  struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    if (pointer->public_id.serial == id.serial) {
      if (val->key == LEME_INPUT_SETTING_SCROLL_FACTOR) {
        pointer->scroll_factor = val->factor;
        return LIBINPUT_CONFIG_STATUS_SUCCESS;
      }
      if (!wlr_input_device_is_libinput(pointer->device)) {
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
      struct libinput_device *dev =
          wlr_libinput_get_device_handle(pointer->device);
      if (dev == NULL) {
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
      switch (val->key) {
      case LEME_INPUT_SETTING_ACCEL_PROFILE:
        return libinput_device_config_accel_set_profile(dev, val->profile);
      case LEME_INPUT_SETTING_ACCEL_SPEED:
        return libinput_device_config_accel_set_speed(dev, val->speed);
      case LEME_INPUT_SETTING_NATURAL_SCROLL:
        return libinput_device_config_scroll_set_natural_scroll_enabled(
            dev, val->boolean ? 1 : 0);
      case LEME_INPUT_SETTING_LEFT_HANDED:
        return libinput_device_config_left_handed_set(dev,
                                                      val->boolean ? 1 : 0);
      case LEME_INPUT_SETTING_TAP:
        return libinput_device_config_tap_set_enabled(
            dev, val->boolean ? LIBINPUT_CONFIG_TAP_ENABLED
                              : LIBINPUT_CONFIG_TAP_DISABLED);
      case LEME_INPUT_SETTING_DISABLE_WHILE_TYPING:
        return libinput_device_config_dwt_set_enabled(
            dev, val->boolean ? LIBINPUT_CONFIG_DWT_ENABLED
                              : LIBINPUT_CONFIG_DWT_DISABLED);
      case LEME_INPUT_SETTING_CLICK_METHOD:
        return libinput_device_config_click_set_method(
            dev, leme_input_click_method(val->choice));
      case LEME_INPUT_SETTING_SCROLL_METHOD:
        return libinput_device_config_scroll_set_method(
            dev, leme_input_scroll_method(val->choice));
      case LEME_INPUT_SETTING_SCROLL_BUTTON:
        return libinput_device_config_scroll_set_button(dev,
                                                        BTN_LEFT + val->choice);
      case LEME_INPUT_SETTING_MIDDLE_EMULATION:
        return libinput_device_config_middle_emulation_set_enabled(
            dev, val->boolean ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                              : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
      case LEME_INPUT_SETTING_TAP_BUTTON_MAP:
        return libinput_device_config_tap_set_button_map(
            dev, leme_input_tap_button_map(val->choice));
      case LEME_INPUT_SETTING_TAP_DRAG:
        return libinput_device_config_tap_set_drag_enabled(
            dev, val->boolean ? LIBINPUT_CONFIG_DRAG_ENABLED
                              : LIBINPUT_CONFIG_DRAG_DISABLED);
      case LEME_INPUT_SETTING_DRAG_LOCK:
        return libinput_device_config_tap_set_drag_lock_enabled(
            dev, leme_input_drag_lock(val->choice));
      case LEME_INPUT_SETTING_DRAG_FINGERS:
        return libinput_device_config_3fg_drag_set_enabled(
            dev, leme_input_drag_fingers(val->fingers));
      default:
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
    }
  }
  return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

bool leme_input_resolve_target(const struct leme_server *server,
                               struct leme_public_id id,
                               struct leme_input_target_info *info) {
  if (server == NULL || info == NULL) {
    return false;
  }
  if (leme_input_pointer_resolve_id(server, id, info)) {
    return true;
  }
  if (leme_input_keyboard_resolve_id(server, id, info)) {
    return true;
  }
  return false;
}
