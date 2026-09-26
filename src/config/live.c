#include "config/live.h"
#include "config/live-internal.h"
#include "config/config.h"
#include "control/control.h"
#include "control/memory.h"
#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "public/schema.h"
#include "public/server.h"
#include "public/value-internal.h"
#include "render/graphics.h"
#include "render/render.h"
#include "shell/view.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_override_key(bool a_has_target,
                                const struct leme_control_target *a_target,
                                const char *const *a_path, size_t a_path_count,
                                bool b_has_target,
                                const struct leme_control_target *b_target,
                                const char *const *b_path, size_t b_path_count) {
  if (!a_has_target && b_has_target) {
    return -1;
  }
  if (a_has_target && !b_has_target) {
    return 1;
  }
  if (a_has_target && b_has_target) {
    if (a_target->kind != b_target->kind) {
      return a_target->kind < b_target->kind ? -1 : 1;
    }
    if (a_target->id.serial != b_target->id.serial) {
      return a_target->id.serial < b_target->id.serial ? -1 : 1;
    }
    if (a_target->tag_number != b_target->tag_number) {
      return a_target->tag_number < b_target->tag_number ? -1 : 1;
    }
  }
  size_t min_len = a_path_count < b_path_count ? a_path_count : b_path_count;
  for (size_t i = 0; i < min_len; i++) {
    int c = strcmp(a_path[i], b_path[i]);
    if (c != 0) {
      return c;
    }
  }
  if (a_path_count != b_path_count) {
    return a_path_count < b_path_count ? -1 : 1;
  }
  return 0;
}

bool leme_config_live_init(struct leme_server *server) {
  if (server == NULL || server->config == NULL) {
    return false;
  }
  if (server->config_store != NULL) {
    return true;
  }
  struct leme_config_store *store = calloc(1, sizeof(*store));
  if (store == NULL) {
    return false;
  }
  if (leme_public_budget_create((size_t)256 * 1024U, NULL, &store->account) !=
      LEME_PUBLIC_OK) {
    free(store);
    return false;
  }
  if (leme_public_builder_create_budget(store->account,
                                        (size_t)256 * 1024U,
                                        &store->builder) != LEME_PUBLIC_OK) {
    leme_public_budget_unref(store->account);
    free(store);
    return false;
  }
  if (leme_public_builder_seal(store->builder, NULL, 0) != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(store->builder);
    leme_public_budget_unref(store->account);
    free(store);
    return false;
  }
  store->baseline = server->config;
  if (leme_config_effective_copy(store->baseline, NULL, &store->effective,
                                 NULL) != LEME_CONTROL_OK) {
    leme_public_builder_destroy(store->builder);
    leme_public_budget_unref(store->account);
    free(store);
    return false;
  }
  store->server = server;
  server->config = store->effective;
  server->config_store = store;
  leme_public_server_config_changed(server);
  return true;
}

void leme_config_store_clear_overrides(struct leme_config_store *store) {
  if (store == NULL) {
    return;
  }
  for (size_t i = 0; i < store->override_count; ++i) {
    for (size_t p = 0; p < store->overrides[i].path_count; ++p) {
      free(store->overrides[i].path[p]);
    }
    free((void *)store->overrides[i].path);
  }
  store->override_count = 0;
  leme_public_server_config_changed(store->server);
  if (store->builder != NULL) {
    leme_public_builder_destroy(store->builder);
    store->builder = NULL;
  }
  if (store->account != NULL) {
    if (leme_public_builder_create_budget(store->account,
                                          (size_t)256 * 1024U,
                                          &store->builder) == LEME_PUBLIC_OK) {
      (void)leme_public_builder_seal(store->builder, NULL, 0);
    }
  }
}

void leme_config_live_finish(struct leme_server *server) {
  if (server == NULL || server->config_store == NULL) {
    return;
  }
  struct leme_config_store *store = server->config_store;
  leme_config_store_clear_overrides(store);
  free(store->overrides);
  store->overrides = NULL;
  store->override_capacity = 0;
  if (store->builder != NULL) {
    leme_public_builder_destroy(store->builder);
    store->builder = NULL;
  }
  if (store->account != NULL) {
    leme_public_budget_unref(store->account);
    store->account = NULL;
  }
  if (store->effective != NULL) {
    leme_config_destroy(store->effective);
    store->effective = NULL;
  }
  if (store->baseline != NULL) {
    leme_config_destroy(store->baseline);
    store->baseline = NULL;
  }
  server->config = NULL;
  server->config_store = NULL;
  leme_public_server_config_changed(server);
  free(store);
}

enum leme_control_code
leme_config_store_add_override(struct leme_config_store *store,
                               const struct leme_control_target *target,
                               const char *const *path, size_t path_count,
                               const struct leme_public_value *value,
                               struct leme_control_error *error) {
  if (store == NULL || path == NULL || path_count == 0 || value == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_INVALID_ARGUMENT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid override arguments");
    }
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  bool has_target = (target != NULL);
  char **path_copy = (char **)calloc(path_count, sizeof(char *));
  if (path_copy == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_OUT_OF_MEMORY;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "out of memory copying override path");
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < path_count; ++i) {
    path_copy[i] = strdup(path[i]);
    if (path_copy[i] == NULL) {
      for (size_t j = 0; j < i; ++j) {
        free(path_copy[j]);
      }
      free((void *)path_copy);
      if (error != NULL) {
        error->code = LEME_CONTROL_OUT_OF_MEMORY;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       "out of memory duplicating path element");
      }
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }

  size_t insert_idx = 0;
  bool found_match = false;
  for (size_t i = 0; i < store->override_count; ++i) {
    int cmp = compare_override_key(
        has_target, target, (const char *const *)path_copy, path_count,
        store->overrides[i].has_target, &store->overrides[i].target,
        (const char *const *)store->overrides[i].path,
        store->overrides[i].path_count);
    if (cmp == 0) {
      insert_idx = i;
      found_match = true;
      break;
    }
    if (cmp < 0) {
      insert_idx = i;
      break;
    }
    insert_idx = i + 1;
  }

  size_t new_count =
      found_match ? store->override_count : store->override_count + 1;
  struct leme_public_builder *new_builder = NULL;
  if (leme_public_builder_create_budget(store->account,
                                        (size_t)256 * 1024U,
                                        &new_builder) != LEME_PUBLIC_OK) {
    for (size_t j = 0; j < path_count; ++j) {
      free(path_copy[j]);
    }
    free((void *)path_copy);
    if (error != NULL) {
      error->code = LEME_CONTROL_RESOURCE_LIMIT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "out of memory creating override builder");
    }
    return LEME_CONTROL_RESOURCE_LIMIT;
  }

  const struct leme_public_value **roots =
      (const struct leme_public_value **)calloc(new_count, sizeof(*roots));
  if (roots == NULL) {
    leme_public_builder_destroy(new_builder);
    for (size_t j = 0; j < path_count; ++j) {
      free(path_copy[j]);
    }
    free((void *)path_copy);
    if (error != NULL) {
      error->code = LEME_CONTROL_OUT_OF_MEMORY;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "out of memory allocating override values");
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  bool clone_ok = true;
  for (size_t idx = 0; idx < new_count; ++idx) {
    struct leme_public_value *cloned = NULL;
    if (idx == insert_idx) {
      if (leme_public_clone(new_builder, value, &cloned) != LEME_PUBLIC_OK) {
        clone_ok = false;
        break;
      }
    } else {
      size_t src_idx = (!found_match && idx > insert_idx) ? idx - 1 : idx;
      if (leme_public_clone(new_builder, store->overrides[src_idx].value,
                            &cloned) != LEME_PUBLIC_OK) {
        clone_ok = false;
        break;
      }
    }
    roots[idx] = cloned;
  }

  if (!clone_ok ||
      leme_public_builder_seal(new_builder, roots, new_count) !=
          LEME_PUBLIC_OK) {
    free((void *)roots);
    leme_public_builder_destroy(new_builder);
    for (size_t j = 0; j < path_count; ++j) {
      free(path_copy[j]);
    }
    free((void *)path_copy);
    if (error != NULL) {
      error->code = LEME_CONTROL_RESOURCE_LIMIT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "failed to clone or seal override value");
    }
    return LEME_CONTROL_RESOURCE_LIMIT;
  }

  if (!found_match && store->override_count == store->override_capacity) {
    size_t new_cap =
        store->override_capacity == 0 ? 8 : store->override_capacity * 2;
    struct leme_scoped_override *new_arr =
        realloc(store->overrides, new_cap * sizeof(*new_arr));
    if (new_arr == NULL) {
      free((void *)roots);
      leme_public_builder_destroy(new_builder);
      for (size_t j = 0; j < path_count; ++j) {
        free(path_copy[j]);
      }
      free((void *)path_copy);
      if (error != NULL) {
        error->code = LEME_CONTROL_OUT_OF_MEMORY;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       "out of memory resizing override array");
      }
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    store->overrides = new_arr;
    store->override_capacity = new_cap;
  }

  if (found_match) {
    for (size_t j = 0; j < store->overrides[insert_idx].path_count; ++j) {
      free(store->overrides[insert_idx].path[j]);
    }
    free((void *)store->overrides[insert_idx].path);
    store->overrides[insert_idx].path = path_copy;
    store->overrides[insert_idx].path_count = path_count;
  } else {
    for (size_t k = store->override_count; k > insert_idx; --k) {
      store->overrides[k] = store->overrides[k - 1];
    }
    store->overrides[insert_idx] = (struct leme_scoped_override){
        .has_target = has_target,
        .target = has_target ? *target : (struct leme_control_target){0},
        .path = path_copy,
        .path_count = path_count,
        .value = NULL,
    };
    store->override_count = new_count;
  }

  for (size_t idx = 0; idx < new_count; ++idx) {
    store->overrides[idx].value = roots[idx];
  }

  free((void *)roots);
  if (store->builder != NULL) {
    leme_public_builder_destroy(store->builder);
  }
  store->builder = new_builder;
  leme_public_server_config_changed(store->server);
  return LEME_CONTROL_OK;
}

void leme_config_store_drop_target(struct leme_config_store *store,
                                   const struct leme_control_target *target) {
  if (store == NULL || target == NULL || store->override_count == 0) {
    return;
  }
  size_t match_count = 0;
  for (size_t i = 0; i < store->override_count; ++i) {
    const struct leme_scoped_override *ov = &store->overrides[i];
    if (ov->has_target && ov->target.kind == target->kind &&
        ov->target.id.serial == target->id.serial) {
      match_count++;
    }
  }
  if (match_count == 0) {
    return;
  }
  if (match_count == store->override_count) {
    leme_config_store_clear_overrides(store);
    return;
  }

  size_t new_count = store->override_count - match_count;
  struct leme_public_builder *new_builder = NULL;
  if (leme_public_builder_create_budget(store->account,
                                        (size_t)256 * 1024U,
                                        &new_builder) != LEME_PUBLIC_OK) {
    return;
  }

  const struct leme_public_value **roots =
      (const struct leme_public_value **)calloc(new_count, sizeof(*roots));
  if (roots == NULL) {
    leme_public_builder_destroy(new_builder);
    return;
  }

  size_t dst = 0;
  bool clone_ok = true;
  for (size_t i = 0; i < store->override_count; ++i) {
    struct leme_scoped_override *ov = &store->overrides[i];
    if (ov->has_target && ov->target.kind == target->kind &&
        ov->target.id.serial == target->id.serial) {
      continue;
    }
    struct leme_public_value *cloned = NULL;
    if (leme_public_clone(new_builder, ov->value, &cloned) != LEME_PUBLIC_OK) {
      clone_ok = false;
      break;
    }
    roots[dst++] = cloned;
  }

  if (!clone_ok ||
      leme_public_builder_seal(new_builder, roots, new_count) !=
          LEME_PUBLIC_OK) {
    free((void *)roots);
    leme_public_builder_destroy(new_builder);
    return;
  }

  size_t write_idx = 0;
  for (size_t i = 0; i < store->override_count; ++i) {
    struct leme_scoped_override *ov = &store->overrides[i];
    if (ov->has_target && ov->target.kind == target->kind &&
        ov->target.id.serial == target->id.serial) {
      for (size_t p = 0; p < ov->path_count; ++p) {
        free(ov->path[p]);
      }
      free((void *)ov->path);
    } else {
      if (write_idx != i) {
        store->overrides[write_idx] = store->overrides[i];
      }
      store->overrides[write_idx].value = roots[write_idx];
      write_idx++;
    }
  }
  store->override_count = new_count;

  free((void *)roots);
  if (store->builder != NULL) {
    leme_public_builder_destroy(store->builder);
  }
  store->builder = new_builder;
  leme_public_server_config_changed(store->server);
}

const struct leme_scoped_override *
leme_config_store_overrides(const struct leme_config_store *store,
                            size_t *out_count) {
  if (out_count != NULL) {
    *out_count = store != NULL ? store->override_count : 0;
  }
  return store != NULL ? store->overrides : NULL;
}

struct leme_config *
leme_config_store_baseline(const struct leme_config_store *store) {
  return store != NULL ? store->baseline : NULL;
}

struct leme_config *
leme_config_store_effective(const struct leme_config_store *store) {
  return store != NULL ? store->effective : NULL;
}

enum leme_live_setting_id {
  LEME_LIVE_STYLE_GAP,
  LEME_LIVE_STYLE_BORDER_WIDTH,
  LEME_LIVE_STYLE_CORNER_RADIUS,
  LEME_LIVE_STYLE_BLUR,
  LEME_LIVE_STYLE_BORDER_ACTIVE,
  LEME_LIVE_STYLE_BORDER_INACTIVE,
  LEME_LIVE_STYLE_OPACITY_ACTIVE,
  LEME_LIVE_STYLE_OPACITY_INACTIVE,
  LEME_LIVE_STYLE_FULLSCREEN_COVERS,
  LEME_LIVE_OUTPUT_CROSS_FOCUS,
  LEME_LIVE_OUTPUT_CROSS_MOVE,
  LEME_LIVE_OUTPUT_CROSS_DRAG,
  LEME_LIVE_OUTPUT_WARP_CURSOR,
  LEME_LIVE_CURSOR_THEME,
  LEME_LIVE_CURSOR_SIZE,
  LEME_LIVE_POINTER_ACCEL_PROFILE,
  LEME_LIVE_POINTER_ACCEL_SPEED,
  LEME_LIVE_POINTER_NATURAL_SCROLL,
  LEME_LIVE_POINTER_LEFT_HANDED,
  LEME_LIVE_POINTER_TAP,
  LEME_LIVE_GESTURES_MODE,
  LEME_LIVE_GESTURES_FINGERS,
  LEME_LIVE_GESTURES_DISTANCE,
  LEME_LIVE_GESTURES_THRESHOLD,
  LEME_LIVE_GESTURES_DECELERATION,
  LEME_LIVE_GESTURES_VELOCITY_WINDOW,
  LEME_LIVE_PUBLICATION_ACTIVATION,
  LEME_LIVE_SETTING_COUNT
};

struct leme_config_prepared_set {
  enum leme_control_opcode opcode;
  enum leme_live_setting_id setting_id;
  char **path;
  size_t path_count;
  const struct leme_public_value *value;
  struct leme_public_builder *builder;
  struct leme_public_budget *account;
  union {
    int int_val;
    double num_val;
    bool bool_val;
    char *str_val;
    float color_val[4];
  } parsed;
  bool is_noop;
  struct wlr_xcursor_manager *prepared_cursor;
};

static enum leme_control_code set_err(struct leme_control_error *error,
                                      enum leme_control_code code,
                                      const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%s", msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

static int find_live_setting(const char *const *path, size_t path_count) {
  if (path == NULL || path_count < 2 || path_count > 3) {
    return -1;
  }
  if (path_count == 2) {
    if (strcmp(path[0], "style") == 0) {
      if (strcmp(path[1], "gap") == 0) return LEME_LIVE_STYLE_GAP;
      if (strcmp(path[1], "border_width") == 0) return LEME_LIVE_STYLE_BORDER_WIDTH;
      if (strcmp(path[1], "corner_radius") == 0) return LEME_LIVE_STYLE_CORNER_RADIUS;
      if (strcmp(path[1], "blur") == 0) return LEME_LIVE_STYLE_BLUR;
      if (strcmp(path[1], "border_active") == 0) return LEME_LIVE_STYLE_BORDER_ACTIVE;
      if (strcmp(path[1], "border_inactive") == 0) return LEME_LIVE_STYLE_BORDER_INACTIVE;
      if (strcmp(path[1], "opacity_active") == 0) return LEME_LIVE_STYLE_OPACITY_ACTIVE;
      if (strcmp(path[1], "opacity_inactive") == 0) return LEME_LIVE_STYLE_OPACITY_INACTIVE;
      if (strcmp(path[1], "fullscreen_covers") == 0) return LEME_LIVE_STYLE_FULLSCREEN_COVERS;
      return -1;
    }
    if (strcmp(path[0], "output_policy") == 0) {
      if (strcmp(path[1], "cross_output_focus") == 0) return LEME_LIVE_OUTPUT_CROSS_FOCUS;
      if (strcmp(path[1], "cross_output_move") == 0) return LEME_LIVE_OUTPUT_CROSS_MOVE;
      if (strcmp(path[1], "cross_output_drag") == 0) return LEME_LIVE_OUTPUT_CROSS_DRAG;
      if (strcmp(path[1], "warp_cursor") == 0) return LEME_LIVE_OUTPUT_WARP_CURSOR;
      return -1;
    }
    if (strcmp(path[0], "cursor") == 0) {
      if (strcmp(path[1], "theme") == 0) return LEME_LIVE_CURSOR_THEME;
      if (strcmp(path[1], "size") == 0) return LEME_LIVE_CURSOR_SIZE;
      return -1;
    }
    if (strcmp(path[0], "pointer") == 0) {
      if (strcmp(path[1], "accel_profile") == 0) return LEME_LIVE_POINTER_ACCEL_PROFILE;
      if (strcmp(path[1], "accel_speed") == 0) return LEME_LIVE_POINTER_ACCEL_SPEED;
      if (strcmp(path[1], "natural_scroll") == 0) return LEME_LIVE_POINTER_NATURAL_SCROLL;
      if (strcmp(path[1], "left_handed") == 0) return LEME_LIVE_POINTER_LEFT_HANDED;
      if (strcmp(path[1], "tap") == 0) return LEME_LIVE_POINTER_TAP;
      return -1;
    }
    if (strcmp(path[0], "publication") == 0) {
      if (strcmp(path[1], "activation") == 0) return LEME_LIVE_PUBLICATION_ACTIVATION;
      return -1;
    }
  } else if (path_count == 3) {
    if (strcmp(path[0], "gestures") == 0 && strcmp(path[1], "workspace_switch") == 0) {
      if (strcmp(path[2], "mode") == 0) return LEME_LIVE_GESTURES_MODE;
      if (strcmp(path[2], "fingers") == 0) return LEME_LIVE_GESTURES_FINGERS;
      if (strcmp(path[2], "distance") == 0) return LEME_LIVE_GESTURES_DISTANCE;
      if (strcmp(path[2], "threshold") == 0) return LEME_LIVE_GESTURES_THRESHOLD;
      if (strcmp(path[2], "deceleration") == 0) return LEME_LIVE_GESTURES_DECELERATION;
      if (strcmp(path[2], "velocity_window") == 0) return LEME_LIVE_GESTURES_VELOCITY_WINDOW;
      return -1;
    }
  }
  return -1;
}

static bool is_unwritable_section(const char *name) {
  if (name == NULL) return false;
  return strcmp(name, "animation") == 0 ||
         strcmp(name, "animations") == 0 ||
         strcmp(name, "binding") == 0 ||
         strcmp(name, "bindings") == 0 ||
         strcmp(name, "environment") == 0 ||
         strcmp(name, "startup") == 0 ||
         strcmp(name, "tags") == 0 ||
         strcmp(name, "window_rules") == 0 ||
         strcmp(name, "modes") == 0 ||
         strcmp(name, "scratchpads") == 0 ||
         strcmp(name, "keyboard") == 0 ||
         strcmp(name, "outputs") == 0 ||
         strcmp(name, "config_errors") == 0;
}

static bool parse_hex_color(struct leme_public_text text, float out[4]) {
  if (text.data == NULL || text.length < 2 || text.data[0] != '#') {
    return false;
  }
  size_t hex_len = text.length - 1;
  if (hex_len != 6 && hex_len != 8) {
    return false;
  }
  for (size_t i = 0; i < hex_len; ++i) {
    if (!isxdigit((unsigned char)text.data[i + 1])) {
      return false;
    }
  }
  unsigned int ch[4] = {0, 0, 0, 255};
  for (size_t i = 0; i < hex_len / 2; i++) {
    char pair[3] = {text.data[i * 2 + 1], text.data[i * 2 + 2], '\0'};
    ch[i] = (unsigned int)strtoul(pair, NULL, 16);
  }
  for (int i = 0; i < 4; i++) {
    out[i] = (float)ch[i] / 255.0f;
  }
  for (int i = 0; i < 3; i++) {
    out[i] *= out[3];
  }
  return true;
}

static bool color_equal(const float a[4], const float b[4]) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void cleanup_prepared_set(struct leme_config_prepared_set *prep) {
  if (prep == NULL) {
    return;
  }
  if (prep->prepared_cursor != NULL) {
    leme_desktop_discard_cursor_config(prep->prepared_cursor);
    prep->prepared_cursor = NULL;
  }
  if (prep->setting_id == LEME_LIVE_CURSOR_THEME) {
    free(prep->parsed.str_val);
    prep->parsed.str_val = NULL;
  }
  if (prep->path != NULL) {
    for (size_t i = 0; i < prep->path_count; i++) {
      free(prep->path[i]);
    }
    free((void *)prep->path);
    prep->path = NULL;
  }
  if (prep->builder != NULL) {
    leme_public_builder_destroy(prep->builder);
    prep->builder = NULL;
  }
  leme_control_free(prep);
}

enum leme_control_code
leme_config_live_prepare_set(struct leme_server *server,
                             const char *const *path, size_t path_count,
                             const struct leme_public_value *value,
                             struct leme_public_budget *account,
                             struct leme_control_prepared **out,
                             struct leme_control_error *error) {
  if (out == NULL) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "null out pointer");
  }
  *out = NULL;
  if (server == NULL || path == NULL || path_count == 0 || value == NULL) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid arguments");
  }
  if (server->config_store == NULL) {
    if (!leme_config_live_init(server)) {
      return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "failed to initialize config store");
    }
  }

  int setting_idx = find_live_setting(path, path_count);
  if (setting_idx < 0) {
    if (is_unwritable_section(path[0])) {
      return set_err(error, LEME_CONTROL_UNSUPPORTED, "unsupported unwritable config path");
    }
    return set_err(error, LEME_CONTROL_NOT_FOUND, "setting not found");
  }

  enum leme_live_setting_id id = (enum leme_live_setting_id)setting_idx;

  if (id == LEME_LIVE_STYLE_CORNER_RADIUS || id == LEME_LIVE_STYLE_BLUR) {
    if (!leme_graphics_effects_supported(server)) {
      return set_err(error, LEME_CONTROL_UNSUPPORTED, "effects not supported at runtime");
    }
  }
  if (id == LEME_LIVE_CURSOR_THEME || id == LEME_LIVE_CURSOR_SIZE) {
    if (!leme_desktop_has_cursor(server)) {
      return set_err(error, LEME_CONTROL_UNSUPPORTED, "cursor system not initialized");
    }
  }

  struct leme_config_prepared_set *prep =
      leme_control_alloc(account, sizeof(struct leme_config_prepared_set));
  if (prep == NULL) {
    return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory allocating prepared set");
  }
  memset(prep, 0, sizeof(*prep));
  prep->opcode = LEME_CONTROL_OP_SET_CONFIG;
  prep->setting_id = id;
  prep->account = account;

  switch (id) {
  case LEME_LIVE_STYLE_GAP:
  case LEME_LIVE_STYLE_BORDER_WIDTH:
  case LEME_LIVE_STYLE_CORNER_RADIUS:
  case LEME_LIVE_STYLE_BLUR:
  case LEME_LIVE_CURSOR_SIZE:
  case LEME_LIVE_GESTURES_FINGERS:
  case LEME_LIVE_GESTURES_VELOCITY_WINDOW: {
    int64_t ival = 0;
    if (leme_public_as_integer(value, &ival) != LEME_PUBLIC_OK) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected integer value");
    }
    int64_t min_bound = 0;
    int64_t max_bound = 2147483647;
    if (id == LEME_LIVE_STYLE_BLUR) {
      max_bound = 64;
    } else if (id == LEME_LIVE_CURSOR_SIZE) {
      min_bound = 1;
      max_bound = 512;
    } else if (id == LEME_LIVE_GESTURES_VELOCITY_WINDOW) {
      min_bound = 1;
    }
    if (ival < min_bound || ival > max_bound) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "integer out of range");
    }
    if (id == LEME_LIVE_GESTURES_FINGERS && (ival == 1 || ival == 2)) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "disallowed finger count");
    }
    prep->parsed.int_val = (int)ival;
    break;
  }
  case LEME_LIVE_STYLE_OPACITY_ACTIVE:
  case LEME_LIVE_STYLE_OPACITY_INACTIVE:
  case LEME_LIVE_POINTER_ACCEL_SPEED:
  case LEME_LIVE_GESTURES_DISTANCE:
  case LEME_LIVE_GESTURES_THRESHOLD:
  case LEME_LIVE_GESTURES_DECELERATION: {
    double dval = 0.0;
    if (leme_public_as_number(value, &dval) != LEME_PUBLIC_OK) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected number value");
    }
    if (!isfinite(dval)) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "nonfinite number value");
    }
    if (id == LEME_LIVE_STYLE_OPACITY_ACTIVE || id == LEME_LIVE_STYLE_OPACITY_INACTIVE) {
      if (dval < 0.0 || dval > 1.0) {
        leme_control_free(prep);
        return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "opacity out of range 0..1");
      }
    } else if (id == LEME_LIVE_POINTER_ACCEL_SPEED) {
      if (dval < -1.0 || dval > 1.0) {
        leme_control_free(prep);
        return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "accel speed out of range -1..1");
      }
    } else if (id == LEME_LIVE_GESTURES_DISTANCE) {
      if (dval <= 0.0) {
        leme_control_free(prep);
        return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "distance must be positive");
      }
    } else if (id == LEME_LIVE_GESTURES_THRESHOLD || id == LEME_LIVE_GESTURES_DECELERATION) {
      if (dval <= 0.0 || dval >= 1.0) {
        leme_control_free(prep);
        return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "value must be strictly between 0 and 1");
      }
    }
    prep->parsed.num_val = dval;
    break;
  }
  case LEME_LIVE_OUTPUT_CROSS_FOCUS:
  case LEME_LIVE_OUTPUT_CROSS_MOVE:
  case LEME_LIVE_OUTPUT_CROSS_DRAG:
  case LEME_LIVE_OUTPUT_WARP_CURSOR:
  case LEME_LIVE_POINTER_NATURAL_SCROLL:
  case LEME_LIVE_POINTER_LEFT_HANDED:
  case LEME_LIVE_POINTER_TAP: {
    if (leme_public_kind(value) != LEME_PUBLIC_BOOLEAN) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected boolean value");
    }
    bool bval = false;
    leme_public_as_bool(value, &bval);
    prep->parsed.bool_val = bval;
    break;
  }
  case LEME_LIVE_STYLE_BORDER_ACTIVE:
  case LEME_LIVE_STYLE_BORDER_INACTIVE: {
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected color string");
    }
    struct leme_public_text txt = {0};
    leme_public_as_text(value, &txt);
    if (!parse_hex_color(txt, prep->parsed.color_val)) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid color format");
    }
    break;
  }
  case LEME_LIVE_STYLE_FULLSCREEN_COVERS: {
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string for fullscreen covers");
    }
    struct leme_public_text txt = {0};
    leme_public_as_text(value, &txt);
    if (txt.length == 4 && memcmp(txt.data, "none", 4) == 0) {
      prep->parsed.int_val = LEME_FULLSCREEN_COVERS_NONE;
    } else if (txt.length == 3 && memcmp(txt.data, "top", 3) == 0) {
      prep->parsed.int_val = LEME_FULLSCREEN_COVERS_TOP;
    } else if (txt.length == 7 && memcmp(txt.data, "overlay", 7) == 0) {
      prep->parsed.int_val = LEME_FULLSCREEN_COVERS_OVERLAY;
    } else {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid fullscreen covers enum");
    }
    break;
  }
  case LEME_LIVE_CURSOR_THEME: {
    if (leme_public_kind(value) == LEME_PUBLIC_NULL) {
      prep->parsed.str_val = NULL;
    } else if (leme_public_kind(value) == LEME_PUBLIC_STRING) {
      struct leme_public_text txt = {0};
      leme_public_as_text(value, &txt);
      prep->parsed.str_val = strndup(txt.data, txt.length);
      if (prep->parsed.str_val == NULL) {
        leme_control_free(prep);
        return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory");
      }
    } else {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string or null for cursor theme");
    }
    break;
  }
  case LEME_LIVE_POINTER_ACCEL_PROFILE: {
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string for accel profile");
    }
    struct leme_public_text txt = {0};
    leme_public_as_text(value, &txt);
    if (txt.length == 8 && memcmp(txt.data, "adaptive", 8) == 0) {
      prep->parsed.int_val = LEME_POINTER_ACCEL_ADAPTIVE;
    } else if (txt.length == 4 && memcmp(txt.data, "flat", 4) == 0) {
      prep->parsed.int_val = LEME_POINTER_ACCEL_FLAT;
    } else {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid accel profile enum");
    }
    break;
  }
  case LEME_LIVE_GESTURES_MODE: {
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string for gesture mode");
    }
    struct leme_public_text txt = {0};
    leme_public_as_text(value, &txt);
    if (txt.length == 6 && memcmp(txt.data, "single", 6) == 0) {
      prep->parsed.int_val = LEME_WORKSPACE_GESTURE_SINGLE;
    } else if (txt.length == 5 && memcmp(txt.data, "scrub", 5) == 0) {
      prep->parsed.int_val = LEME_WORKSPACE_GESTURE_SCRUB;
    } else if (txt.length == 4 && memcmp(txt.data, "free", 4) == 0) {
      prep->parsed.int_val = LEME_WORKSPACE_GESTURE_FREE;
    } else {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid gesture mode enum");
    }
    break;
  }
  case LEME_LIVE_PUBLICATION_ACTIVATION: {
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string for activation policy");
    }
    struct leme_public_text txt = {0};
    leme_public_as_text(value, &txt);
    if (txt.length == 6 && memcmp(txt.data, "follow", 6) == 0) {
      prep->parsed.int_val = LEME_ACTIVATION_FOLLOW;
    } else if (txt.length == 6 && memcmp(txt.data, "urgent", 6) == 0) {
      prep->parsed.int_val = LEME_ACTIVATION_URGENT;
    } else if (txt.length == 6 && memcmp(txt.data, "ignore", 6) == 0) {
      prep->parsed.int_val = LEME_ACTIVATION_IGNORE;
    } else {
      leme_control_free(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid activation enum");
    }
    break;
  }
  default:
    leme_control_free(prep);
    return set_err(error, LEME_CONTROL_UNSUPPORTED, "unsupported setting");
  }

  struct leme_config *cur = server->config;
  bool noop = false;
  switch (id) {
  case LEME_LIVE_STYLE_GAP:
    noop = (cur->gap == prep->parsed.int_val);
    break;
  case LEME_LIVE_STYLE_BORDER_WIDTH:
    noop = (cur->border_width == prep->parsed.int_val);
    break;
  case LEME_LIVE_STYLE_CORNER_RADIUS:
    noop = (cur->corner_radius == prep->parsed.int_val);
    break;
  case LEME_LIVE_STYLE_BLUR:
    noop = (cur->blur == prep->parsed.int_val);
    break;
  case LEME_LIVE_STYLE_BORDER_ACTIVE:
    noop = color_equal(cur->border_active, prep->parsed.color_val);
    break;
  case LEME_LIVE_STYLE_BORDER_INACTIVE:
    noop = color_equal(cur->border_inactive, prep->parsed.color_val);
    break;
  case LEME_LIVE_STYLE_OPACITY_ACTIVE:
    noop = (cur->opacity_active == prep->parsed.num_val);
    break;
  case LEME_LIVE_STYLE_OPACITY_INACTIVE:
    noop = (cur->opacity_inactive == prep->parsed.num_val);
    break;
  case LEME_LIVE_STYLE_FULLSCREEN_COVERS:
    noop = (cur->fullscreen_covers == (enum leme_fullscreen_coverage)prep->parsed.int_val);
    break;
  case LEME_LIVE_OUTPUT_CROSS_FOCUS:
    noop = (cur->output_policy.cross_output_focus == prep->parsed.bool_val);
    break;
  case LEME_LIVE_OUTPUT_CROSS_MOVE:
    noop = (cur->output_policy.cross_output_move == prep->parsed.bool_val);
    break;
  case LEME_LIVE_OUTPUT_CROSS_DRAG:
    noop = (cur->output_policy.cross_output_drag == prep->parsed.bool_val);
    break;
  case LEME_LIVE_OUTPUT_WARP_CURSOR:
    noop = (cur->output_policy.warp_cursor == prep->parsed.bool_val);
    break;
  case LEME_LIVE_CURSOR_THEME:
    noop = ((cur->cursor.theme == NULL && prep->parsed.str_val == NULL) ||
            (cur->cursor.theme != NULL && prep->parsed.str_val != NULL &&
             strcmp(cur->cursor.theme, prep->parsed.str_val) == 0));
    break;
  case LEME_LIVE_CURSOR_SIZE:
    noop = (cur->cursor.size == prep->parsed.int_val);
    break;
  case LEME_LIVE_POINTER_ACCEL_PROFILE:
    noop = (cur->pointer_defaults.profile == (enum leme_pointer_accel_profile)prep->parsed.int_val);
    break;
  case LEME_LIVE_POINTER_ACCEL_SPEED:
    noop = (cur->pointer_defaults.speed == prep->parsed.num_val);
    break;
  case LEME_LIVE_POINTER_NATURAL_SCROLL:
    noop = (cur->pointer_defaults.natural_scroll == prep->parsed.bool_val);
    break;
  case LEME_LIVE_POINTER_LEFT_HANDED:
    noop = (cur->pointer_defaults.left_handed == prep->parsed.bool_val);
    break;
  case LEME_LIVE_POINTER_TAP:
    noop = (cur->pointer_defaults.tap == prep->parsed.bool_val);
    break;
  case LEME_LIVE_GESTURES_MODE:
    noop = (cur->gestures.workspace_switch.mode == (enum leme_workspace_gesture_mode)prep->parsed.int_val);
    break;
  case LEME_LIVE_GESTURES_FINGERS:
    noop = (cur->gestures.workspace_switch.fingers == (uint32_t)prep->parsed.int_val);
    break;
  case LEME_LIVE_GESTURES_DISTANCE:
    noop = (cur->gestures.workspace_switch.distance == prep->parsed.num_val);
    break;
  case LEME_LIVE_GESTURES_THRESHOLD:
    noop = (cur->gestures.workspace_switch.threshold == prep->parsed.num_val);
    break;
  case LEME_LIVE_GESTURES_DECELERATION:
    noop = (cur->gestures.workspace_switch.deceleration == prep->parsed.num_val);
    break;
  case LEME_LIVE_GESTURES_VELOCITY_WINDOW:
    noop = (cur->gestures.workspace_switch.velocity_window_ms == (uint32_t)prep->parsed.int_val);
    break;
  case LEME_LIVE_PUBLICATION_ACTIVATION:
    noop = (cur->publication.activation == (enum leme_activation_policy)prep->parsed.int_val);
    break;
  default:
    break;
  }
  prep->is_noop = noop;

  if (!noop && (id == LEME_LIVE_CURSOR_THEME || id == LEME_LIVE_CURSOR_SIZE)) {
    const char *theme = (id == LEME_LIVE_CURSOR_THEME) ? prep->parsed.str_val : cur->cursor.theme;
    int size = (id == LEME_LIVE_CURSOR_SIZE) ? prep->parsed.int_val : cur->cursor.size;
    prep->prepared_cursor = leme_desktop_prepare_cursor_config(server, theme, size);
    if (prep->prepared_cursor == NULL) {
      cleanup_prepared_set(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "failed to prepare cursor theme");
    }
  }

  struct leme_public_builder *b = NULL;
  if (leme_public_builder_create_budget(account, 16384, &b) != LEME_PUBLIC_OK) {
    cleanup_prepared_set(prep);
    return set_err(error, LEME_CONTROL_RESOURCE_LIMIT, "budget exceeded");
  }

  struct leme_public_value *cloned = NULL;
  if (leme_public_clone(b, value, &cloned) != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(b);
    cleanup_prepared_set(prep);
    return set_err(error, LEME_CONTROL_RESOURCE_LIMIT, "failed to clone value");
  }

  const struct leme_public_value *roots[1] = {cloned};
  if (leme_public_builder_seal(b, roots, 1) != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(b);
    cleanup_prepared_set(prep);
    return set_err(error, LEME_CONTROL_RESOURCE_LIMIT, "failed to seal builder");
  }

  prep->builder = b;
  prep->value = cloned;

  prep->path = (char **)calloc(path_count, sizeof(char *));
  if (prep->path == NULL) {
    cleanup_prepared_set(prep);
    return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory allocating path");
  }
  for (size_t i = 0; i < path_count; i++) {
    prep->path[i] = strdup(path[i]);
    if (prep->path[i] == NULL) {
      cleanup_prepared_set(prep);
      return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory duplicating path");
    }
    prep->path_count++;
  }

  *out = (struct leme_control_prepared *)prep;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_config_live_prepare(struct leme_server *server,
                         const struct leme_control_intent *intents,
                         size_t count, struct leme_public_budget *account,
                         struct leme_control_prepared **out,
                         struct leme_control_error *error) {
  if (count != 1 || intents == NULL) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid intent count");
  }
  const struct leme_public_value *args_val = intents[0].args;
  if (args_val == NULL || leme_public_kind(args_val) != LEME_PUBLIC_ARRAY ||
      leme_public_length(args_val) != 2) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "set-config requires [path, value]");
  }
  const struct leme_public_value *path_val = leme_public_at(args_val, 0);
  const struct leme_public_value *new_val = leme_public_at(args_val, 1);
  if (path_val == NULL || leme_public_kind(path_val) != LEME_PUBLIC_ARRAY) {
    return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected array for config path");
  }
  size_t p_len = leme_public_length(path_val);
  if (p_len < 2 || p_len > 3) {
    return set_err(error, LEME_CONTROL_NOT_FOUND, "invalid config path length");
  }
  char *p_strs[3] = {NULL};
  enum leme_control_code code = LEME_CONTROL_OK;
  for (size_t i = 0; i < p_len; i++) {
    const struct leme_public_value *seg = leme_public_at(path_val, i);
    struct leme_public_text txt = {0};
    if (leme_public_as_text(seg, &txt) != LEME_PUBLIC_OK) {
      code = set_err(error, LEME_CONTROL_TYPE_ERROR,
                     "path segments must be strings");
      goto cleanup;
    }
    if (memchr(txt.data, '\0', txt.length) != NULL) {
      code = set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "path segments must not contain NUL");
      goto cleanup;
    }
    p_strs[i] = strndup(txt.data, txt.length);
    if (p_strs[i] == NULL) {
      code = set_err(error, LEME_CONTROL_OUT_OF_MEMORY,
                     "out of memory duplicating config path");
      goto cleanup;
    }
  }

  code = leme_config_live_prepare_set(
      server, (const char *const *)p_strs, p_len, new_val, account, out, error);
cleanup:
  for (size_t i = 0; i < p_len; i++) {
    free(p_strs[i]);
  }
  return code;
}

enum leme_control_code
leme_config_live_execute_one(struct leme_server *server,
                             struct leme_control_prepared *prepared,
                             size_t index, enum leme_control_outcome *outcome,
                             struct leme_control_error *error) {
  (void)index;
  if (server == NULL || prepared == NULL || outcome == NULL) {
    if (outcome != NULL) *outcome = LEME_CONTROL_FAILED;
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid arguments");
  }
  struct leme_config_prepared_set *prep =
      (struct leme_config_prepared_set *)prepared;

  if (prep->is_noop) {
    *outcome = LEME_CONTROL_NOOP;
    return LEME_CONTROL_OK;
  }

  leme_public_server_config_changed(server);
  struct leme_config *cfg = server->config;
  switch (prep->setting_id) {
  case LEME_LIVE_STYLE_GAP:
    cfg->gap = prep->parsed.int_val;
    leme_view_arrange(server);
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_BORDER_WIDTH:
    cfg->border_width = prep->parsed.int_val;
    leme_view_arrange(server);
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_CORNER_RADIUS:
    cfg->corner_radius = prep->parsed.int_val;
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_BLUR:
    cfg->blur = prep->parsed.int_val;
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_BORDER_ACTIVE:
    memcpy(cfg->border_active, prep->parsed.color_val, sizeof(float) * 4);
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_BORDER_INACTIVE:
    memcpy(cfg->border_inactive, prep->parsed.color_val, sizeof(float) * 4);
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_OPACITY_ACTIVE:
    cfg->opacity_active = prep->parsed.num_val;
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_OPACITY_INACTIVE:
    cfg->opacity_inactive = prep->parsed.num_val;
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_STYLE_FULLSCREEN_COVERS:
    cfg->fullscreen_covers = (enum leme_fullscreen_coverage)prep->parsed.int_val;
    leme_render_apply_fullscreen_coverage(server);
    leme_view_refresh_fullscreen(server);
    leme_view_arrange(server);
    leme_render_refresh_views(server);
    break;
  case LEME_LIVE_OUTPUT_CROSS_FOCUS:
    cfg->output_policy.cross_output_focus = prep->parsed.bool_val;
    break;
  case LEME_LIVE_OUTPUT_CROSS_MOVE:
    cfg->output_policy.cross_output_move = prep->parsed.bool_val;
    break;
  case LEME_LIVE_OUTPUT_CROSS_DRAG:
    cfg->output_policy.cross_output_drag = prep->parsed.bool_val;
    break;
  case LEME_LIVE_OUTPUT_WARP_CURSOR:
    cfg->output_policy.warp_cursor = prep->parsed.bool_val;
    break;
  case LEME_LIVE_CURSOR_THEME:
    if (prep->prepared_cursor != NULL) {
      leme_desktop_commit_cursor_config(server, prep->prepared_cursor);
      prep->prepared_cursor = NULL;
    }
    free(cfg->cursor.theme);
    cfg->cursor.theme = prep->parsed.str_val != NULL ? strdup(prep->parsed.str_val) : NULL;
    break;
  case LEME_LIVE_CURSOR_SIZE:
    if (prep->prepared_cursor != NULL) {
      leme_desktop_commit_cursor_config(server, prep->prepared_cursor);
      prep->prepared_cursor = NULL;
    }
    cfg->cursor.size = prep->parsed.int_val;
    break;
  case LEME_LIVE_POINTER_ACCEL_PROFILE:
    cfg->pointer_defaults.profile = (enum leme_pointer_accel_profile)prep->parsed.int_val;
    leme_input_apply_pointer_config(server, cfg);
    break;
  case LEME_LIVE_POINTER_ACCEL_SPEED:
    cfg->pointer_defaults.speed = prep->parsed.num_val;
    leme_input_apply_pointer_config(server, cfg);
    break;
  case LEME_LIVE_POINTER_NATURAL_SCROLL:
    cfg->pointer_defaults.natural_scroll = prep->parsed.bool_val;
    leme_input_apply_pointer_config(server, cfg);
    break;
  case LEME_LIVE_POINTER_LEFT_HANDED:
    cfg->pointer_defaults.left_handed = prep->parsed.bool_val;
    leme_input_apply_pointer_config(server, cfg);
    break;
  case LEME_LIVE_POINTER_TAP:
    cfg->pointer_defaults.tap = prep->parsed.bool_val;
    leme_input_apply_pointer_config(server, cfg);
    break;
  case LEME_LIVE_GESTURES_MODE:
    cfg->gestures.workspace_switch.mode = (enum leme_workspace_gesture_mode)prep->parsed.int_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_GESTURES_FINGERS:
    cfg->gestures.workspace_switch.fingers = (uint32_t)prep->parsed.int_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_GESTURES_DISTANCE:
    cfg->gestures.workspace_switch.distance = prep->parsed.num_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_GESTURES_THRESHOLD:
    cfg->gestures.workspace_switch.threshold = prep->parsed.num_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_GESTURES_DECELERATION:
    cfg->gestures.workspace_switch.deceleration = prep->parsed.num_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_GESTURES_VELOCITY_WINDOW:
    cfg->gestures.workspace_switch.velocity_window_ms = (uint32_t)prep->parsed.int_val;
    leme_input_workspace_gesture_reset(server);
    break;
  case LEME_LIVE_PUBLICATION_ACTIVATION:
    cfg->publication.activation = (enum leme_activation_policy)prep->parsed.int_val;
    break;
  default:
    break;
  }

  enum leme_control_code ov_code = leme_config_store_add_override(
      server->config_store, NULL, (const char *const *)prep->path,
      prep->path_count, prep->value, error);
  if (ov_code != LEME_CONTROL_OK) {
    return ov_code;
  }

  leme_public_server_invalidate(server);
  *outcome = LEME_CONTROL_APPLIED;
  return LEME_CONTROL_OK;
}

void leme_config_live_discard(struct leme_server *server,
                              struct leme_control_prepared *prepared) {
  (void)server;
  cleanup_prepared_set((struct leme_config_prepared_set *)prepared);
}
