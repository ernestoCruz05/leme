#include "output/control.h"

#include "config/live.h"
#include "control/action.h"
#include "control/error.h"
#include "core/server.h"
#include "output/output.h"
#include "public/model.h"
#include "public/value.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_management_v1.h>

static bool parse_transform(struct leme_public_text text,
                            enum wl_output_transform *out) {
  if (text.length == 6 && memcmp(text.data, "normal", 6) == 0) {
    *out = WL_OUTPUT_TRANSFORM_NORMAL;
    return true;
  }
  if (text.length == 2 && memcmp(text.data, "90", 2) == 0) {
    *out = WL_OUTPUT_TRANSFORM_90;
    return true;
  }
  if (text.length == 3 && memcmp(text.data, "180", 3) == 0) {
    *out = WL_OUTPUT_TRANSFORM_180;
    return true;
  }
  if (text.length == 3 && memcmp(text.data, "270", 3) == 0) {
    *out = WL_OUTPUT_TRANSFORM_270;
    return true;
  }
  if (text.length == 7 && memcmp(text.data, "flipped", 7) == 0) {
    *out = WL_OUTPUT_TRANSFORM_FLIPPED;
    return true;
  }
  if (text.length == 10 && memcmp(text.data, "flipped-90", 10) == 0) {
    *out = WL_OUTPUT_TRANSFORM_FLIPPED_90;
    return true;
  }
  if (text.length == 11 && memcmp(text.data, "flipped-180", 11) == 0) {
    *out = WL_OUTPUT_TRANSFORM_FLIPPED_180;
    return true;
  }
  if (text.length == 11 && memcmp(text.data, "flipped-270", 11) == 0) {
    *out = WL_OUTPUT_TRANSFORM_FLIPPED_270;
    return true;
  }
  return false;
}

static enum leme_control_code configure(struct leme_server *server,
                                        struct leme_output *target,
                                        const struct leme_public_value *patch,
                                        bool *applied,
                                        struct leme_control_error *error) {
  if (patch == NULL || leme_public_kind(patch) != LEME_PUBLIC_OBJECT) {
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "patch must be an object");
  }

  size_t patch_len = leme_public_length(patch);
  if (patch_len == 0)
    return LEME_CONTROL_OK;

  bool has_mode = false;
  bool has_position = false;
  bool has_scale = false;
  bool has_transform = false;

  int64_t mode_width = 0;
  int64_t mode_height = 0;
  int64_t mode_refresh = 0;
  int64_t pos_x = 0;
  int64_t pos_y = 0;
  double scale_val = 0.0;
  enum wl_output_transform transform_val = WL_OUTPUT_TRANSFORM_NORMAL;
  struct wlr_output_mode *hardware_mode = NULL;

  for (size_t i = 0; i < patch_len; i++) {
    struct leme_public_text key = leme_public_key_at(patch, i);
    const struct leme_public_value *val = leme_public_member_at(patch, i);

    if (key.length == 4 && memcmp(key.data, "mode", 4) == 0) {
      if (has_mode || leme_public_kind(val) != LEME_PUBLIC_OBJECT) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid mode");
      }
      if (leme_public_length(val) != 3) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "mode must have width, height, refresh_mhz");
      }
      const struct leme_public_value *w_val =
          leme_public_get(val, LEME_PUBLIC_TEXT("width"));
      const struct leme_public_value *h_val =
          leme_public_get(val, LEME_PUBLIC_TEXT("height"));
      const struct leme_public_value *r_val =
          leme_public_get(val, LEME_PUBLIC_TEXT("refresh_mhz"));

      if (w_val == NULL || h_val == NULL || r_val == NULL) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "missing required mode keys");
      }
      if (leme_public_as_integer(w_val, &mode_width) != LEME_PUBLIC_OK ||
          leme_public_as_integer(h_val, &mode_height) != LEME_PUBLIC_OK ||
          leme_public_as_integer(r_val, &mode_refresh) != LEME_PUBLIC_OK) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "mode values must be integers");
      }
      if (mode_width <= 0 || mode_width > INT_MAX || mode_height <= 0 ||
          mode_height > INT_MAX || mode_refresh <= 0 ||
          mode_refresh > INT_MAX) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "mode values must be positive integers");
      }
      if (!wl_list_empty(&target->wlr_output->modes)) {
        struct wlr_output_mode *m;
        wl_list_for_each(m, &target->wlr_output->modes, link) {
          if (m->width == (int)mode_width && m->height == (int)mode_height) {
            long long diff =
                llabs((long long)m->refresh - (long long)mode_refresh);
            if (diff <= 1000) {
              hardware_mode = m;
              break;
            }
          }
        }
        if (hardware_mode == NULL) {
          return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                     "unsupported output mode");
        }
      }
      has_mode = true;
    } else if (key.length == 8 && memcmp(key.data, "position", 8) == 0) {
      if (has_position || leme_public_kind(val) != LEME_PUBLIC_OBJECT) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid position");
      }
      if (leme_public_length(val) != 2) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "position must have x and y");
      }
      const struct leme_public_value *x_val =
          leme_public_get(val, LEME_PUBLIC_TEXT("x"));
      const struct leme_public_value *y_val =
          leme_public_get(val, LEME_PUBLIC_TEXT("y"));
      if (x_val == NULL || y_val == NULL) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "missing required position keys");
      }
      if (leme_public_as_integer(x_val, &pos_x) != LEME_PUBLIC_OK ||
          leme_public_as_integer(y_val, &pos_y) != LEME_PUBLIC_OK) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "position values must be integers");
      }
      if (pos_x < -1000000 || pos_x > 1000000 || pos_y < -1000000 ||
          pos_y > 1000000) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "position out of bounds");
      }
      has_position = true;
    } else if (key.length == 5 && memcmp(key.data, "scale", 5) == 0) {
      if (has_scale ||
          leme_public_as_number(val, &scale_val) != LEME_PUBLIC_OK) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid scale");
      }
      if (!isfinite(scale_val) || scale_val < 0.5 || scale_val > 4.0) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "scale must be between 0.5 and 4.0");
      }
      has_scale = true;
    } else if (key.length == 9 && memcmp(key.data, "transform", 9) == 0) {
      struct leme_public_text tr_text = {0};
      if (has_transform ||
          leme_public_as_text(val, &tr_text) != LEME_PUBLIC_OK) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid transform");
      }
      if (!parse_transform(tr_text, &transform_val)) {
        return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "unsupported output transform");
      }
      has_transform = true;
    } else {
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "unsupported output patch key");
    }
  }

  struct wlr_output_configuration_v1 *config =
      wlr_output_configuration_v1_create();
  if (config == NULL) {
    return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }

  struct leme_output *cand;
  wl_list_for_each(cand, &server->outputs, link) {
    struct wlr_output_configuration_head_v1 *head =
        wlr_output_configuration_head_v1_create(config, cand->wlr_output);
    if (head == NULL) {
      wlr_output_configuration_v1_destroy(config);
      return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    head->state.enabled = cand->wlr_output->enabled;
    if (cand == target) {
      head->state.x = has_position ? (int)pos_x : cand->layout_x;
      head->state.y = has_position ? (int)pos_y : cand->layout_y;
      head->state.scale =
          has_scale ? (float)scale_val : cand->wlr_output->scale;
      head->state.transform =
          has_transform ? transform_val : cand->wlr_output->transform;
      if (has_mode) {
        head->state.mode = hardware_mode;
        head->state.custom_mode.width = (int)mode_width;
        head->state.custom_mode.height = (int)mode_height;
        head->state.custom_mode.refresh = (int)mode_refresh;
      } else {
        head->state.mode = cand->wlr_output->current_mode;
        head->state.custom_mode.width = cand->wlr_output->width;
        head->state.custom_mode.height = cand->wlr_output->height;
        head->state.custom_mode.refresh = cand->wlr_output->refresh;
      }
    } else {
      head->state.x = cand->layout_x;
      head->state.y = cand->layout_y;
      head->state.scale = cand->wlr_output->scale;
      head->state.transform = cand->wlr_output->transform;
      head->state.mode = cand->wlr_output->current_mode;
      head->state.custom_mode.width = cand->wlr_output->width;
      head->state.custom_mode.height = cand->wlr_output->height;
      head->state.custom_mode.refresh = cand->wlr_output->refresh;
    }
    head->state.adaptive_sync_enabled =
        cand->wlr_output->adaptive_sync_status ==
        WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
  }

  if (leme_output_control_heads_overlap(config)) {
    wlr_output_configuration_v1_destroy(config);
    return leme_control_refuse(
        error, LEME_CONTROL_INVALID_ARGUMENT,
        "output configuration overlaps with existing output");
  }

  if (!leme_output_control_test_configuration(server, config)) {
    wlr_output_configuration_v1_destroy(config);
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "output configuration rejected by backend");
  }

  if (leme_output_control_configuration_matches_current(server, config)) {
    wlr_output_configuration_v1_destroy(config);
    return LEME_CONTROL_OK;
  }
  const bool committed =
      leme_output_control_commit_configuration(server, config);
  wlr_output_configuration_v1_destroy(config);
  if (!committed)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "backend configuration commit failed");
  leme_output_publish_configuration(server);
  if (server->config_store != NULL) {
    const struct leme_control_target override = {.kind = LEME_PUBLIC_OUTPUT,
                                                 .id = target->public_id};
    for (size_t i = 0; i < patch_len; i++) {
      struct leme_public_text key = leme_public_key_at(patch, i);
      char key_str[32];
      (void)snprintf(key_str, sizeof(key_str), "%.*s", (int)key.length,
                     key.data);
      const char *path[] = {key_str};
      (void)leme_config_store_add_override(
          server->config_store, &override, path, 1,
          leme_public_member_at(patch, i), NULL);
    }
  }
  *applied = true;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_output_control_run(struct leme_server *server,
                        const struct leme_control_call *call, bool *applied,
                        struct leme_control_error *error) {
  *applied = false;
  struct leme_output *target =
      leme_output_by_public_id(server, call->target->id);
  if (target == NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "output not found");
  const struct leme_public_value *arg = call->count > 0 ? call->args[0] : NULL;
  if (call->opcode == LEME_CONTROL_OP_CONFIGURE_OUTPUT)
    return configure(server, target, arg, applied, error);
  bool power = false;
  if (leme_public_as_bool(arg, &power) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "power must be a boolean");
  if (target->power_on == power)
    return LEME_CONTROL_OK;
  if (!leme_output_set_power(target, power))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "failed to set output power");
  *applied = true;
  return LEME_CONTROL_OK;
}
