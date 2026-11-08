#include "input/control.h"

#include "config/config.h"
#include "config/live.h"
#include "control/action.h"
#include "control/error.h"
#include "core/server.h"
#include "input/input.h"
#include "input/internal.h"
#include "ipc/ipc.h"
#include "public/server.h"
#include "public/value.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum leme_control_code set_mode(struct leme_server *server,
                                       const struct leme_public_value *value,
                                       bool *applied,
                                       struct leme_control_error *error) {
  struct leme_public_text text = {0};
  if (leme_public_as_text(value, &text) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "mode must be string");
  if (text.length == 0 || text.length >= 64)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid mode name");
  char name[64];
  memcpy(name, text.data, text.length);
  name[text.length] = '\0';
  bool found = false;
  for (size_t i = 0; i < server->mode_count && !found; i++)
    found = strcmp(server->modes[i].name, name) == 0;
  if (!found)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND, "mode not found");
  if (server->active_mode != NULL &&
      strcmp(server->active_mode->name, name) == 0)
    return LEME_CONTROL_OK;
  if (!leme_input_set_mode(server, name))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "failed to set mode");
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
set_keyboard_layout(struct leme_server *server,
                    const struct leme_public_value *value, bool *applied,
                    struct leme_control_error *error) {
  struct leme_public_text text = {0};
  if (leme_public_as_text(value, &text) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "layout must be string");
  if (text.length == 0 || text.length >= 128)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid layout name");
  char label[128];
  memcpy(label, text.data, text.length);
  label[text.length] = '\0';
  size_t index = 0;
  bool ambiguous = false;
  if (!leme_input_find_keyboard_layout(server, label, &index, &ambiguous))
    return ambiguous ? leme_control_refuse(
                           error, LEME_CONTROL_INVALID_ARGUMENT,
                           "ambiguous duplicate keyboard layout label")
                     : leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                                           "keyboard layout not found");
  if (server->keyboard_layout == (xkb_layout_index_t)index)
    return LEME_CONTROL_OK;
  if (!leme_input_select_keyboard_layout(server, label))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "failed to select keyboard layout");
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code set_input(struct leme_server *server,
                                        const struct leme_control_call *call,
                                        bool *applied,
                                        struct leme_control_error *error) {
  if (call->count != 2)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid arguments");
  const struct leme_public_value *key_val = call->args[0];
  const struct leme_public_value *val_val = call->args[1];
  struct leme_public_text key_text = {0};
  if (leme_public_as_text(key_val, &key_text) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "key must be string");
  if (val_val == NULL)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR, "missing value");
  char key_str[32];
  (void)snprintf(key_str, sizeof(key_str), "%.*s", (int)key_text.length,
                 key_text.data);

  struct leme_input_setting_val setting = {0};
  if (strcmp(key_str, "accel_profile") == 0) {
    setting.key = LEME_INPUT_SETTING_ACCEL_PROFILE;
    if (leme_public_kind(val_val) != LEME_PUBLIC_STRING) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "accel_profile must be string");
    }
    struct leme_public_text ptext = {0};
    leme_public_as_text(val_val, &ptext);
    if (ptext.length == 4 && memcmp(ptext.data, "flat", 4) == 0) {
      setting.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
    } else if (ptext.length == 8 && memcmp(ptext.data, "adaptive", 8) == 0) {
      setting.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    } else {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid accel_profile value");
    }
  } else if (strcmp(key_str, "accel_speed") == 0) {
    setting.key = LEME_INPUT_SETTING_ACCEL_SPEED;
    if (leme_public_kind(val_val) != LEME_PUBLIC_NUMBER) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "accel_speed must be number");
    }
    double sp = 0;
    leme_public_as_number(val_val, &sp);
    if (sp < -1.0 || sp > 1.0) {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "accel_speed out of range");
    }
    setting.speed = sp;
  } else if (strcmp(key_str, "natural_scroll") == 0) {
    setting.key = LEME_INPUT_SETTING_NATURAL_SCROLL;
    if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "natural_scroll must be boolean");
    }
    leme_public_as_bool(val_val, &setting.boolean);
  } else if (strcmp(key_str, "left_handed") == 0) {
    setting.key = LEME_INPUT_SETTING_LEFT_HANDED;
    if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "left_handed must be boolean");
    }
    leme_public_as_bool(val_val, &setting.boolean);
  } else if (strcmp(key_str, "tap") == 0) {
    setting.key = LEME_INPUT_SETTING_TAP;
    if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "tap must be boolean");
    }
    leme_public_as_bool(val_val, &setting.boolean);
  } else if (strcmp(key_str, "disable_while_typing") == 0 ||
             strcmp(key_str, "middle_emulation") == 0 ||
             strcmp(key_str, "tap_drag") == 0) {
    setting.key = key_str[0] == 'd'   ? LEME_INPUT_SETTING_DISABLE_WHILE_TYPING
                  : key_str[0] == 'm' ? LEME_INPUT_SETTING_MIDDLE_EMULATION
                                      : LEME_INPUT_SETTING_TAP_DRAG;
    if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "value must be boolean");
    }
    leme_public_as_bool(val_val, &setting.boolean);
  } else if (strcmp(key_str, "click_method") == 0 ||
             strcmp(key_str, "scroll_method") == 0 ||
             strcmp(key_str, "scroll_button") == 0 ||
             strcmp(key_str, "tap_button_map") == 0 ||
             strcmp(key_str, "drag_lock") == 0) {
    enum leme_pointer_choice choice = LEME_POINTER_CHOICE_DRAG_LOCK;
    setting.key = LEME_INPUT_SETTING_DRAG_LOCK;
    if (strcmp(key_str, "click_method") == 0) {
      choice = LEME_POINTER_CHOICE_CLICK_METHOD;
      setting.key = LEME_INPUT_SETTING_CLICK_METHOD;
    } else if (strcmp(key_str, "scroll_method") == 0) {
      choice = LEME_POINTER_CHOICE_SCROLL_METHOD;
      setting.key = LEME_INPUT_SETTING_SCROLL_METHOD;
    } else if (strcmp(key_str, "scroll_button") == 0) {
      choice = LEME_POINTER_CHOICE_SCROLL_BUTTON;
      setting.key = LEME_INPUT_SETTING_SCROLL_BUTTON;
    } else if (strcmp(key_str, "tap_button_map") == 0) {
      choice = LEME_POINTER_CHOICE_TAP_BUTTON_MAP;
      setting.key = LEME_INPUT_SETTING_TAP_BUTTON_MAP;
    }
    struct leme_public_text text = {0};
    if (leme_public_as_text(val_val, &text) != LEME_PUBLIC_OK) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "value must be string");
    }
    if (!leme_pointer_choice_find(choice, text.data, text.length,
                                  &setting.choice)) {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid value");
    }
  } else if (strcmp(key_str, "drag_fingers") == 0) {
    setting.key = LEME_INPUT_SETTING_DRAG_FINGERS;
    int64_t fingers = 0;
    if (leme_public_as_integer(val_val, &fingers) != LEME_PUBLIC_OK) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "drag_fingers must be integer");
    }
    if (fingers != 0 && fingers != 3 && fingers != 4) {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "drag_fingers must be 0, 3 or 4");
    }
    if (fingers != 0 && server->config != NULL &&
        (leme_config_swipe_fingers(server->config) & (1u << fingers)) != 0) {
      return leme_control_refuse(
          error, LEME_CONTROL_INVALID_ARGUMENT,
          "swipes with that finger count switch tags or are bound");
    }
    setting.fingers = (uint32_t)fingers;
  } else if (strcmp(key_str, "scroll_factor") == 0) {
    setting.key = LEME_INPUT_SETTING_SCROLL_FACTOR;
    if (leme_public_as_number(val_val, &setting.factor) != LEME_PUBLIC_OK) {
      return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "scroll_factor must be number");
    }
    if (!(setting.factor > 0.0) ||
        setting.factor > LEME_POINTER_SCROLL_FACTOR_MAX) {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "scroll_factor out of range");
    }
  } else {
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "unknown input property");
  }

  struct leme_input_target_info tinfo = {0};
  if (!leme_input_resolve_target(server, call->target->id, &tinfo))
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "input device not found");
  if (tinfo.is_keyboard)
    return leme_control_refuse(
        error, LEME_CONTROL_INVALID_ARGUMENT,
        "keyboard device does not support pointer setting");
  if (!tinfo.is_libinput && setting.key != LEME_INPUT_SETTING_SCROLL_FACTOR)
    return leme_control_refuse(
        error, LEME_CONTROL_UNSUPPORTED,
        "device does not support libinput configuration");
  if (setting.key == LEME_INPUT_SETTING_ACCEL_PROFILE) {
    if (setting.profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT && !tinfo.flat) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "accel_profile flat not supported");
    }
    if (setting.profile == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE &&
        !tinfo.adaptive) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "accel_profile adaptive not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_ACCEL_SPEED) {
    if (!tinfo.has_accel) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "accel_speed not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_NATURAL_SCROLL) {
    if (!tinfo.has_natural_scroll) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "natural_scroll not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_LEFT_HANDED) {
    if (!tinfo.has_left_handed) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "left_handed not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_TAP ||
             setting.key == LEME_INPUT_SETTING_TAP_BUTTON_MAP ||
             setting.key == LEME_INPUT_SETTING_TAP_DRAG ||
             setting.key == LEME_INPUT_SETTING_DRAG_LOCK) {
    if (!tinfo.has_tap) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "tap not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_DISABLE_WHILE_TYPING) {
    if (!tinfo.has_disable_while_typing) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "disable_while_typing not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_MIDDLE_EMULATION) {
    if (!tinfo.has_middle_emulation) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "middle_emulation not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_CLICK_METHOD) {
    if (tinfo.click_methods == 0) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "click_method not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_SCROLL_METHOD) {
    if (tinfo.scroll_methods == 0) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "scroll_method not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_SCROLL_BUTTON) {
    if (!tinfo.has_scroll_button) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "scroll_button not supported");
    }
  } else if (setting.key == LEME_INPUT_SETTING_DRAG_FINGERS) {
    if (tinfo.drag_finger_count < 3 ||
        setting.fingers > tinfo.drag_finger_count) {
      return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                                 "drag_fingers not supported");
    }
  }

  bool same = false;
  if (setting.key == LEME_INPUT_SETTING_ACCEL_PROFILE)
    same = tinfo.current_accel_profile == setting.profile;
  else if (setting.key == LEME_INPUT_SETTING_ACCEL_SPEED)
    same = fabs(tinfo.current_accel_speed - setting.speed) < 1e-6;
  else if (setting.key == LEME_INPUT_SETTING_NATURAL_SCROLL)
    same = tinfo.current_natural_scroll == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_LEFT_HANDED)
    same = tinfo.current_left_handed == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_TAP)
    same = tinfo.current_tap == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_DISABLE_WHILE_TYPING)
    same = tinfo.current_disable_while_typing == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_MIDDLE_EMULATION)
    same = tinfo.current_middle_emulation == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_TAP_DRAG)
    same = tinfo.current_tap_drag == setting.boolean;
  else if (setting.key == LEME_INPUT_SETTING_CLICK_METHOD)
    same = tinfo.current_click_method == setting.choice;
  else if (setting.key == LEME_INPUT_SETTING_SCROLL_METHOD)
    same = tinfo.current_scroll_method == setting.choice;
  else if (setting.key == LEME_INPUT_SETTING_SCROLL_BUTTON)
    same = tinfo.has_current_scroll_button &&
           tinfo.current_scroll_button == setting.choice;
  else if (setting.key == LEME_INPUT_SETTING_TAP_BUTTON_MAP)
    same = tinfo.current_tap_button_map == setting.choice;
  else if (setting.key == LEME_INPUT_SETTING_DRAG_LOCK)
    same = tinfo.current_drag_lock == setting.choice;
  else if (setting.key == LEME_INPUT_SETTING_DRAG_FINGERS)
    same = tinfo.current_drag_fingers == setting.fingers;
  else if (setting.key == LEME_INPUT_SETTING_SCROLL_FACTOR)
    same = fabs(tinfo.current_scroll_factor - setting.factor) < 1e-9;
  if (same)
    return LEME_CONTROL_OK;
  if (leme_input_apply_pointer_setting(server, call->target->id, &setting) !=
      LIBINPUT_CONFIG_STATUS_SUCCESS)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "libinput configuration failed");
  if (server->config_store != NULL) {
    const struct leme_control_target override = {.kind = LEME_PUBLIC_INPUT,
                                                 .id = call->target->id};
    const char *path[] = {key_str};
    (void)leme_config_store_add_override(server->config_store, &override, path,
                                         1, val_val, NULL);
  }
  leme_public_server_invalidate(server);
  leme_ipc_invalidate(server);
  *applied = true;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_input_control_run(struct leme_server *server,
                       const struct leme_control_call *call, bool *applied,
                       struct leme_control_error *error) {
  *applied = false;
  const struct leme_public_value *arg = call->count > 0 ? call->args[0] : NULL;
  switch (call->opcode) {
  case LEME_CONTROL_OP_SET_MODE:
    return set_mode(server, arg, applied, error);
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    return set_keyboard_layout(server, arg, applied, error);
  case LEME_CONTROL_OP_SET_INPUT:
    return set_input(server, call, applied, error);
  default:
    return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported action opcode");
  }
}
