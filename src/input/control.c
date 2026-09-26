#include "input/control.h"

#include "config/config.h"
#include "config/live.h"
#include "control/action.h"
#include "control/error.h"
#include "control/memory.h"
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

struct input_control_item {
  uint64_t serial;
  bool is_noop;
};

struct input_control_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  size_t count;
  struct input_control_item *items;
  struct leme_input_setting_val setting;
  char key_str[32];
  const struct leme_public_value *value;
  char mode_name[64];
  char layout_label[128];
  bool is_noop;
};

static enum leme_control_code
set_preflight_error(struct leme_control_error *error,
                    enum leme_control_code code, const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%s", msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

enum leme_control_code leme_input_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  if (server == NULL || intents == NULL || count == 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  const enum leme_control_opcode opcode = intents[0].opcode;

  if (opcode == LEME_CONTROL_OP_SET_MODE) {
    if (count != 1) {
      return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                                 "single target required");
    }
    if (intents[0].args == NULL ||
        leme_public_kind(intents[0].args) != LEME_PUBLIC_STRING) {
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "mode must be string");
    }
    struct leme_public_text text = {0};
    leme_public_as_text(intents[0].args, &text);
    if (text.length == 0 || text.length >= 64) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid mode name");
    }
    bool found = false;
    for (size_t i = 0; i < server->mode_count; i++) {
      if (strncmp(server->modes[i].name, text.data, text.length) == 0 &&
          server->modes[i].name[text.length] == '\0') {
        found = true;
        break;
      }
    }
    if (!found) {
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "mode not found");
    }

    struct input_control_prepared *prep =
        leme_control_alloc(account, sizeof(struct input_control_prepared));
    if (prep == NULL) {
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(prep, 0, sizeof(*prep));
    prep->opcode = opcode;
    prep->account = account;
    prep->count = 1;
    memcpy(prep->mode_name, text.data, text.length);
    prep->mode_name[text.length] = '\0';
    prep->is_noop = (server->active_mode != NULL &&
                     strcmp(server->active_mode->name, prep->mode_name) == 0);
    *out = (struct leme_control_prepared *)prep;
    return LEME_CONTROL_OK;
  }

  if (opcode == LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT) {
    if (count != 1) {
      return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                                 "single target required");
    }
    if (intents[0].args == NULL ||
        leme_public_kind(intents[0].args) != LEME_PUBLIC_STRING) {
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "layout must be string");
    }
    struct leme_public_text text = {0};
    leme_public_as_text(intents[0].args, &text);
    if (text.length == 0 || text.length >= 128) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid layout name");
    }
    char label[128];
    memcpy(label, text.data, text.length);
    label[text.length] = '\0';

    size_t target_idx = 0;
    bool ambiguous = false;
    if (!leme_input_find_keyboard_layout(server, label, &target_idx,
                                         &ambiguous)) {
      if (ambiguous) {
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "ambiguous duplicate keyboard layout label");
      }
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "keyboard layout not found");
    }

    struct input_control_prepared *prep =
        leme_control_alloc(account, sizeof(struct input_control_prepared));
    if (prep == NULL) {
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(prep, 0, sizeof(*prep));
    prep->opcode = opcode;
    prep->account = account;
    prep->count = 1;
    memcpy(prep->layout_label, label, sizeof(prep->layout_label));
    prep->is_noop = (server->keyboard_layout == (xkb_layout_index_t)target_idx);
    *out = (struct leme_control_prepared *)prep;
    return LEME_CONTROL_OK;
  }

  if (opcode == LEME_CONTROL_OP_SET_INPUT) {
    if (intents[0].args == NULL ||
        leme_public_kind(intents[0].args) != LEME_PUBLIC_ARRAY ||
        leme_public_length(intents[0].args) != 2) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid arguments");
    }
    const struct leme_public_value *key_val =
        leme_public_at(intents[0].args, 0);
    const struct leme_public_value *val_val =
        leme_public_at(intents[0].args, 1);
    if (key_val == NULL || leme_public_kind(key_val) != LEME_PUBLIC_STRING) {
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "key must be string");
    }
    if (val_val == NULL) {
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "missing value");
    }

    struct leme_public_text key_text = {0};
    leme_public_as_text(key_val, &key_text);
    char key_str[32];
    (void)snprintf(key_str, sizeof(key_str), "%.*s", (int)key_text.length,
                   key_text.data);

    struct leme_input_setting_val setting = {0};
    if (strcmp(key_str, "accel_profile") == 0) {
      setting.key = LEME_INPUT_SETTING_ACCEL_PROFILE;
      if (leme_public_kind(val_val) != LEME_PUBLIC_STRING) {
        return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                   "accel_profile must be string");
      }
      struct leme_public_text ptext = {0};
      leme_public_as_text(val_val, &ptext);
      if (ptext.length == 4 && memcmp(ptext.data, "flat", 4) == 0) {
        setting.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
      } else if (ptext.length == 8 && memcmp(ptext.data, "adaptive", 8) == 0) {
        setting.profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
      } else {
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid accel_profile value");
      }
    } else if (strcmp(key_str, "accel_speed") == 0) {
      setting.key = LEME_INPUT_SETTING_ACCEL_SPEED;
      if (leme_public_kind(val_val) != LEME_PUBLIC_NUMBER) {
        return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                   "accel_speed must be number");
      }
      double sp = 0;
      leme_public_as_number(val_val, &sp);
      if (sp < -1.0 || sp > 1.0) {
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "accel_speed out of range");
      }
      setting.speed = sp;
    } else if (strcmp(key_str, "natural_scroll") == 0) {
      setting.key = LEME_INPUT_SETTING_NATURAL_SCROLL;
      if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
        return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                   "natural_scroll must be boolean");
      }
      leme_public_as_bool(val_val, &setting.boolean);
    } else if (strcmp(key_str, "left_handed") == 0) {
      setting.key = LEME_INPUT_SETTING_LEFT_HANDED;
      if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
        return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                   "left_handed must be boolean");
      }
      leme_public_as_bool(val_val, &setting.boolean);
    } else if (strcmp(key_str, "tap") == 0) {
      setting.key = LEME_INPUT_SETTING_TAP;
      if (leme_public_kind(val_val) != LEME_PUBLIC_BOOLEAN) {
        return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                   "tap must be boolean");
      }
      leme_public_as_bool(val_val, &setting.boolean);
    } else {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "unknown input property");
    }

    for (size_t i = 0; i < count; i++) {
      if (!intents[i].has_target ||
          intents[i].target.kind != LEME_PUBLIC_INPUT) {
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid input target");
      }
      struct leme_input_target_info tinfo = {0};
      if (!leme_input_resolve_target(server, intents[i].target.id, &tinfo)) {
        return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                   "input device not found");
      }
      if (tinfo.is_keyboard) {
        return set_preflight_error(
            error, LEME_CONTROL_INVALID_ARGUMENT,
            "keyboard device does not support pointer setting");
      }
      if (!tinfo.is_libinput) {
        return set_preflight_error(
            error, LEME_CONTROL_UNSUPPORTED,
            "device does not support libinput configuration");
      }
      if (setting.key == LEME_INPUT_SETTING_ACCEL_PROFILE) {
        if (setting.profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT &&
            !tinfo.flat) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "accel_profile flat not supported");
        }
        if (setting.profile == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE &&
            !tinfo.adaptive) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "accel_profile adaptive not supported");
        }
      } else if (setting.key == LEME_INPUT_SETTING_ACCEL_SPEED) {
        if (!tinfo.has_accel) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "accel_speed not supported");
        }
      } else if (setting.key == LEME_INPUT_SETTING_NATURAL_SCROLL) {
        if (!tinfo.has_natural_scroll) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "natural_scroll not supported");
        }
      } else if (setting.key == LEME_INPUT_SETTING_LEFT_HANDED) {
        if (!tinfo.has_left_handed) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "left_handed not supported");
        }
      } else if (setting.key == LEME_INPUT_SETTING_TAP) {
        if (!tinfo.has_tap) {
          return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                                     "tap not supported");
        }
      }
    }

    struct input_control_prepared *prep =
        leme_control_alloc(account, sizeof(struct input_control_prepared));
    if (prep == NULL) {
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(prep, 0, sizeof(*prep));
    prep->opcode = opcode;
    prep->account = account;
    prep->count = count;
    prep->setting = setting;
    memcpy(prep->key_str, key_str, sizeof(prep->key_str));
    prep->value = val_val;
    prep->items =
        leme_control_alloc(account, count * sizeof(struct input_control_item));
    if (prep->items == NULL) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(prep->items, 0, count * sizeof(struct input_control_item));

    for (size_t i = 0; i < count; i++) {
      prep->items[i].serial = intents[i].target.id.serial;
      struct leme_input_target_info tinfo = {0};
      leme_input_resolve_target(server, intents[i].target.id, &tinfo);
      bool is_noop = false;
      if (setting.key == LEME_INPUT_SETTING_ACCEL_PROFILE) {
        is_noop = (tinfo.current_accel_profile == setting.profile);
      } else if (setting.key == LEME_INPUT_SETTING_ACCEL_SPEED) {
        is_noop = (fabs(tinfo.current_accel_speed - setting.speed) < 1e-6);
      } else if (setting.key == LEME_INPUT_SETTING_NATURAL_SCROLL) {
        is_noop = (tinfo.current_natural_scroll == setting.boolean);
      } else if (setting.key == LEME_INPUT_SETTING_LEFT_HANDED) {
        is_noop = (tinfo.current_left_handed == setting.boolean);
      } else if (setting.key == LEME_INPUT_SETTING_TAP) {
        is_noop = (tinfo.current_tap == setting.boolean);
      }
      prep->items[i].is_noop = is_noop;
    }

    *out = (struct leme_control_prepared *)prep;
    return LEME_CONTROL_OK;
  }

  return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                             "unsupported action opcode");
}

enum leme_control_code
leme_input_control_execute_one(struct leme_server *server,
                               struct leme_control_prepared *prepared,
                               size_t index, enum leme_control_outcome *outcome,
                               struct leme_control_error *error) {
  if (server == NULL || prepared == NULL || outcome == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  struct input_control_prepared *prep =
      (struct input_control_prepared *)prepared;

  if (prep->opcode == LEME_CONTROL_OP_SET_MODE) {
    if (prep->is_noop) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    if (!leme_input_set_mode(server, prep->mode_name)) {
      *outcome = LEME_CONTROL_FAILED;
      return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                 "failed to set mode");
    }
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }

  if (prep->opcode == LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT) {
    if (prep->is_noop) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    if (!leme_input_select_keyboard_layout(server, prep->layout_label)) {
      *outcome = LEME_CONTROL_FAILED;
      return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                 "failed to select keyboard layout");
    }
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }

  if (prep->opcode == LEME_CONTROL_OP_SET_INPUT) {
    if (index >= prep->count) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_INVALID_ARGUMENT;
    }
    struct input_control_item *item = &prep->items[index];
    if (item->is_noop) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }

    struct leme_public_id id = {.serial = item->serial};
    enum libinput_config_status st =
        leme_input_apply_pointer_setting(server, id, &prep->setting);
    if (st != LIBINPUT_CONFIG_STATUS_SUCCESS) {
      *outcome = LEME_CONTROL_FAILED;
      return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                 "libinput configuration failed");
    }

    if (server->config_store != NULL) {
      struct leme_control_target ov_target = {
          .kind = LEME_PUBLIC_INPUT,
          .id = id,
      };
      const char *path[] = {prep->key_str};
      (void)leme_config_store_add_override(server->config_store, &ov_target,
                                           path, 1, prep->value, NULL);
    }
    leme_public_server_invalidate(server);
    leme_ipc_invalidate(server);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }

  *outcome = LEME_CONTROL_FAILED;
  return LEME_CONTROL_UNSUPPORTED;
}

void leme_input_control_discard(struct leme_server *server,
                                struct leme_control_prepared *prepared) {
  (void)server;
  if (prepared == NULL) {
    return;
  }
  struct input_control_prepared *prep =
      (struct input_control_prepared *)prepared;
  if (prep->items != NULL) {
    leme_control_free(prep->items);
  }
  leme_control_free(prep);
}
