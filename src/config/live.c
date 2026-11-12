#include "config/live.h"
#include "config/live-internal.h"
#include "config/config.h"
#include "config/internal.h"
#include "config/option.h"
#include "control/control.h"
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
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_override_key(bool a_has_target,
                                const struct leme_control_target *a_target,
                                const char *const *a_path, size_t a_path_count,
                                bool b_has_target,
                                const struct leme_control_target *b_target,
                                const char *const *b_path,
                                size_t b_path_count) {
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

static bool leme_config_live_init(struct leme_server *server) {
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
  if (leme_public_builder_create((size_t)256 * 1024U, NULL, &store->builder) !=
      LEME_PUBLIC_OK) {
    free(store);
    return false;
  }
  if (leme_public_builder_seal(store->builder, NULL, 0) != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(store->builder);
    free(store);
    return false;
  }
  store->baseline = server->config;
  if (leme_config_effective_copy(store->baseline, &store->effective, NULL) !=
      LEME_CONTROL_OK) {
    leme_public_builder_destroy(store->builder);
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
  if (leme_public_builder_create((size_t)256 * 1024U, NULL, &store->builder) ==
      LEME_PUBLIC_OK) {
    (void)leme_public_builder_seal(store->builder, NULL, 0);
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

enum leme_control_code leme_config_store_add_override(
    struct leme_config_store *store, const struct leme_control_target *target,
    const char *const *path, size_t path_count,
    const struct leme_public_value *value, struct leme_control_error *error) {
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
  if (leme_public_builder_create((size_t)256 * 1024U, NULL, &new_builder) !=
      LEME_PUBLIC_OK) {
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

  if (!clone_ok || leme_public_builder_seal(new_builder, roots, new_count) !=
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
  if (leme_public_builder_create((size_t)256 * 1024U, NULL, &new_builder) !=
      LEME_PUBLIC_OK) {
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

  if (!clone_ok || leme_public_builder_seal(new_builder, roots, new_count) !=
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

struct leme_option_group {
  const char *path[2];
  size_t path_count;
  const struct leme_option_block *block;
  size_t base;
  size_t fields;
};

static const struct leme_option leme_live_gap_outer_options[] = {
    {.name = "top",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_gaps, top),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "right",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_gaps, right),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "bottom",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_gaps, bottom),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "left",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_gaps, left),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
};

static const struct leme_option_block leme_live_gap_outer_block = {
    .name = "gap_outer",
    .options = leme_live_gap_outer_options,
    .count = LEME_OPTION_COUNT(leme_live_gap_outer_options),
};

static const struct leme_option leme_live_pointer_options[] = {
    {.name = "focus_follows_mouse",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, focus_follows_mouse),
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "drag_fingers",
     .kind = LEME_OPTION_U32,
     .offset = offsetof(struct leme_config, pointer_defaults.drag_fingers),
     .min = 0,
     .max = 4,
     .field = LEME_POINTER_DRAG_FINGERS,
     .effect = LEME_OPTION_EFFECT_POINTERS},
};

static const struct leme_option_block leme_live_pointer_block = {
    .name = "pointer",
    .options = leme_live_pointer_options,
    .count = LEME_OPTION_COUNT(leme_live_pointer_options),
};

static const struct leme_option leme_live_swipe_options[] = {
    {.name = "fingers",
     .kind = LEME_OPTION_U32,
     .offset = offsetof(struct leme_workspace_switch_gesture_settings, fingers),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_FINGERS},
};

static const struct leme_option_block leme_live_swipe_block = {
    .name = "workspace_switch",
    .options = leme_live_swipe_options,
    .count = LEME_OPTION_COUNT(leme_live_swipe_options),
};

static const struct leme_option_group leme_live_groups[] = {
    {.path = {"desktop"}, .path_count = 1, .block = &leme_desktop_block},
    {.path = {"style"}, .path_count = 1, .block = &leme_style_block},
    {.path = {"style", "gap_outer"},
     .path_count = 2,
     .block = &leme_live_gap_outer_block,
     .base = offsetof(struct leme_config, gap_outer)},
    {.path = {"output_policy"},
     .path_count = 1,
     .block = &leme_output_policy_block},
    {.path = {"cursor"}, .path_count = 1, .block = &leme_cursor_block},
    {.path = {"pointer"},
     .path_count = 1,
     .block = &leme_pointer_block,
     .base = offsetof(struct leme_config, pointer_defaults),
     .fields = offsetof(struct leme_config, pointer_defaults.fields)},
    {.path = {"pointer"},
     .path_count = 1,
     .block = &leme_live_pointer_block,
     .fields = offsetof(struct leme_config, pointer_defaults.fields)},
    {.path = {"publication"},
     .path_count = 1,
     .block = &leme_publication_block},
    {.path = {"gestures", "workspace_switch"},
     .path_count = 2,
     .block = &leme_workspace_switch_block,
     .base = offsetof(struct leme_config, gestures.workspace_switch)},
    {.path = {"gestures", "workspace_switch"},
     .path_count = 2,
     .block = &leme_live_swipe_block,
     .base = offsetof(struct leme_config, gestures.workspace_switch)},
    {.path = {"gestures", "edges"},
     .path_count = 2,
     .block = &leme_edges_block,
     .base = offsetof(struct leme_config, gestures.edges)},
};

struct leme_config_prepared_set {
  const struct leme_option_group *group;
  const struct leme_option *option;
  char **path;
  size_t path_count;
  const struct leme_public_value *value;
  struct leme_public_builder *builder;
  union leme_option_value parsed;
  bool is_noop;
  struct wlr_xcursor_manager *prepared_cursor;
};

static enum leme_control_code set_err(struct leme_control_error *error,
                                      enum leme_control_code code,
                                      const char *msg) {
  return leme_control_refuse(error, code, msg);
}

static const struct leme_option *
find_live_setting(const char *const *path, size_t path_count,
                  const struct leme_option_group **group) {
  for (size_t i = 0; i < LEME_OPTION_COUNT(leme_live_groups); i++) {
    const struct leme_option_group *candidate = &leme_live_groups[i];

    if (path_count != candidate->path_count + 1 ||
        strcmp(path[0], candidate->path[0]) != 0 ||
        (candidate->path_count == 2 &&
         strcmp(path[1], candidate->path[1]) != 0)) {
      continue;
    }
    for (size_t j = 0; j < candidate->block->count; j++) {
      const struct leme_option *option = &candidate->block->options[j];

      if (option->effect != LEME_OPTION_EFFECT_NONE &&
          strcmp(path[path_count - 1], option->name) == 0) {
        *group = candidate;
        return option;
      }
    }
  }
  return NULL;
}

static bool is_unwritable_section(const char *name) {
  if (name == NULL)
    return false;
  return strcmp(name, "animation") == 0 || strcmp(name, "animations") == 0 ||
         strcmp(name, "binding") == 0 || strcmp(name, "bindings") == 0 ||
         strcmp(name, "environment") == 0 || strcmp(name, "startup") == 0 ||
         strcmp(name, "tags") == 0 || strcmp(name, "window_rules") == 0 ||
         strcmp(name, "modes") == 0 || strcmp(name, "scratchpads") == 0 ||
         strcmp(name, "keyboard") == 0 || strcmp(name, "outputs") == 0 ||
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

static void cleanup_prepared_set(struct leme_config_prepared_set *prep) {
  if (prep == NULL) {
    return;
  }
  if (prep->prepared_cursor != NULL) {
    leme_desktop_discard_cursor_config(prep->prepared_cursor);
    prep->prepared_cursor = NULL;
  }
  if (prep->option->kind == LEME_OPTION_STRING) {
    free(prep->parsed.string);
    prep->parsed.string = NULL;
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
  free(prep);
}

static enum leme_control_code
convert_value(const struct leme_server *server,
              const struct leme_option *option,
              const struct leme_public_value *value,
              union leme_option_value *out, struct leme_control_error *error) {
  struct leme_public_text text = {0};

  switch (option->kind) {
  case LEME_OPTION_INT:
  case LEME_OPTION_U16:
  case LEME_OPTION_U32:
    if (leme_public_as_integer(value, &out->integer) != LEME_PUBLIC_OK) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected integer value");
    }
    if (!leme_option_in_range(option, out)) {
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "integer out of range");
    }
    if ((option->effect == LEME_OPTION_EFFECT_FINGERS ||
         option == &leme_live_pointer_options[1]) &&
        (out->integer == 1 || out->integer == 2)) {
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "disallowed finger count");
    }
    for (size_t i = 0; option->effect == LEME_OPTION_EFFECT_FINGERS &&
                       out->integer != 0 && i < server->config->mode_count;
         i++) {
      const struct leme_mode *mode = &server->config->modes[i];

      for (size_t j = 0; j < mode->binding_count; j++) {
        const struct leme_binding *binding = &mode->bindings[j];

        if (binding->trigger == LEME_BINDING_SWIPE &&
            binding->fingers == out->integer &&
            binding->direction != LEME_GESTURE_UP &&
            binding->direction != LEME_GESTURE_DOWN) {
          return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                         "fingers overlap a swipe binding");
        }
      }
    }
    return LEME_CONTROL_OK;
  case LEME_OPTION_DECIMAL:
    if (leme_public_as_number(value, &out->decimal) != LEME_PUBLIC_OK) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected number value");
    }
    if (!leme_option_in_range(option, out)) {
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "number out of range");
    }
    return LEME_CONTROL_OK;
  case LEME_OPTION_BOOL:
    if (leme_public_kind(value) != LEME_PUBLIC_BOOLEAN) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected boolean value");
    }
    (void)leme_public_as_bool(value, &out->boolean);
    return LEME_CONTROL_OK;
  case LEME_OPTION_COLOR:
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected color string");
    }
    (void)leme_public_as_text(value, &text);
    if (!parse_hex_color(text, out->color)) {
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "invalid color format");
    }
    return LEME_CONTROL_OK;
  case LEME_OPTION_CHOICE:
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string value");
    }
    (void)leme_public_as_text(value, &text);
    for (out->choice = 0; out->choice < option->choice_count; out->choice++) {
      const char *name = option->choices[out->choice];

      if (strlen(name) == text.length &&
          memcmp(name, text.data, text.length) == 0) {
        return LEME_CONTROL_OK;
      }
    }
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid value");
  case LEME_OPTION_STRING:
    if (leme_public_kind(value) == LEME_PUBLIC_NULL) {
      out->string = NULL;
      return LEME_CONTROL_OK;
    }
    if (leme_public_kind(value) != LEME_PUBLIC_STRING) {
      return set_err(error, LEME_CONTROL_TYPE_ERROR, "expected string or null");
    }
    (void)leme_public_as_text(value, &text);
    out->string = strndup(text.data, text.length);
    if (out->string == NULL) {
      return set_err(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory");
    }
    return LEME_CONTROL_OK;
  case LEME_OPTION_CUSTOM:
    break;
  }
  return set_err(error, LEME_CONTROL_UNSUPPORTED, "unsupported setting");
}

static enum leme_control_code prepare_set(struct leme_server *server,
                                          const char *const *path,
                                          size_t path_count,
                                          const struct leme_public_value *value,
                                          struct leme_config_prepared_set **out,
                                          struct leme_control_error *error) {
  const struct leme_option_group *group = NULL;

  *out = NULL;
  if (value == NULL) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT, "invalid arguments");
  }
  if (server->config_store == NULL && !leme_config_live_init(server)) {
    return set_err(error, LEME_CONTROL_OUT_OF_MEMORY,
                   "failed to initialize config store");
  }
  const struct leme_option *option =
      find_live_setting(path, path_count, &group);
  if (option == NULL) {
    if (is_unwritable_section(path[0])) {
      return set_err(error, LEME_CONTROL_UNSUPPORTED,
                     "unsupported unwritable config path");
    }
    return set_err(error, LEME_CONTROL_NOT_FOUND, "setting not found");
  }
  if (option->effect == LEME_OPTION_EFFECT_EFFECTS &&
      !leme_graphics_effects_supported(server)) {
    return set_err(error, LEME_CONTROL_UNSUPPORTED,
                   "effects not supported at runtime");
  }
  if (option->effect == LEME_OPTION_EFFECT_CURSOR &&
      !leme_desktop_has_cursor(server)) {
    return set_err(error, LEME_CONTROL_UNSUPPORTED,
                   "cursor system not initialized");
  }

  struct leme_config_prepared_set *prep = calloc(1, sizeof(*prep));
  if (prep == NULL) {
    return set_err(error, LEME_CONTROL_OUT_OF_MEMORY,
                   "out of memory allocating prepared set");
  }
  prep->group = group;
  prep->option = option;
  const enum leme_control_code converted =
      convert_value(server, option, value, &prep->parsed, error);
  if (converted != LEME_CONTROL_OK) {
    free(prep);
    return converted;
  }

  struct leme_config *cur = server->config;
  const uint32_t fields =
      group->fields == 0
          ? option->field
          : *(const uint32_t *)((const char *)cur + group->fields);

  prep->is_noop =
      (fields & option->field) == option->field &&
      leme_option_equal(option, (const char *)cur + group->base, &prep->parsed);
  if (!prep->is_noop && option->effect == LEME_OPTION_EFFECT_CURSOR) {
    const bool theme = option->kind == LEME_OPTION_STRING;

    prep->prepared_cursor = leme_desktop_prepare_cursor_config(
        server, theme ? prep->parsed.string : cur->cursor.theme,
        theme ? cur->cursor.size : (int)prep->parsed.integer);
    if (prep->prepared_cursor == NULL) {
      cleanup_prepared_set(prep);
      return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                     "failed to prepare cursor theme");
    }
  }

  struct leme_public_builder *b = NULL;
  if (leme_public_builder_create(16384, NULL, &b) != LEME_PUBLIC_OK) {
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
    return set_err(error, LEME_CONTROL_RESOURCE_LIMIT,
                   "failed to seal builder");
  }

  prep->builder = b;
  prep->value = cloned;

  prep->path = (char **)calloc(path_count, sizeof(char *));
  if (prep->path == NULL) {
    cleanup_prepared_set(prep);
    return set_err(error, LEME_CONTROL_OUT_OF_MEMORY,
                   "out of memory allocating path");
  }
  for (size_t i = 0; i < path_count; i++) {
    prep->path[i] = strdup(path[i]);
    if (prep->path[i] == NULL) {
      cleanup_prepared_set(prep);
      return set_err(error, LEME_CONTROL_OUT_OF_MEMORY,
                     "out of memory duplicating path");
    }
    prep->path_count++;
  }

  *out = prep;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
leme_config_live_set(struct leme_server *server, const char *const *path,
                     size_t path_count, const struct leme_public_value *value,
                     bool *applied, struct leme_control_error *error);

enum leme_control_code
leme_config_live_run(struct leme_server *server,
                     const struct leme_control_call *call, bool *applied,
                     struct leme_control_error *error) {
  *applied = false;
  if (call->count != 2) {
    return set_err(error, LEME_CONTROL_INVALID_ARGUMENT,
                   "set-config requires [path, value]");
  }
  const struct leme_public_value *path_val = call->args[0];
  const struct leme_public_value *new_val = call->args[1];
  if (path_val == NULL || leme_public_kind(path_val) != LEME_PUBLIC_ARRAY) {
    return set_err(error, LEME_CONTROL_TYPE_ERROR,
                   "expected array for config path");
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

  code = leme_config_live_set(server, (const char *const *)p_strs, p_len,
                              new_val, applied, error);
cleanup:
  for (size_t i = 0; i < p_len; i++) {
    free(p_strs[i]);
  }
  return code;
}

static void refresh_setting(struct leme_server *server,
                            enum leme_option_effect effect) {
  switch (effect) {
  case LEME_OPTION_EFFECT_NONE:
  case LEME_OPTION_EFFECT_STORE:
  case LEME_OPTION_EFFECT_CURSOR:
    return;
  case LEME_OPTION_EFFECT_LAYOUT:
    leme_view_arrange_instant(server);
    return;
  case LEME_OPTION_EFFECT_ARRANGE:
    leme_view_arrange_instant(server);
    leme_render_refresh_views(server);
    return;
  case LEME_OPTION_EFFECT_REDRAW:
  case LEME_OPTION_EFFECT_EFFECTS:
    leme_render_refresh_views(server);
    return;
  case LEME_OPTION_EFFECT_FULLSCREEN:
    leme_render_apply_fullscreen_coverage(server);
    leme_view_refresh_fullscreen(server);
    leme_view_arrange_instant(server);
    leme_render_refresh_views(server);
    return;
  case LEME_OPTION_EFFECT_POINTERS:
    leme_input_apply_pointer_config(server, server->config);
    return;
  case LEME_OPTION_EFFECT_SWIPE:
    leme_input_workspace_gesture_reset(server);
    return;
  case LEME_OPTION_EFFECT_FINGERS:
    leme_input_workspace_gesture_reset(server);
    leme_input_apply_pointer_config(server, server->config);
    return;
  }
}

static enum leme_control_code apply(struct leme_server *server,
                                    struct leme_config_prepared_set *prep,
                                    bool *applied,
                                    struct leme_control_error *error) {
  if (prep->is_noop) {
    return LEME_CONTROL_OK;
  }

  leme_public_server_config_changed(server);
  struct leme_config *cfg = server->config;

  if (prep->prepared_cursor != NULL) {
    leme_desktop_commit_cursor_config(server, prep->prepared_cursor);
    prep->prepared_cursor = NULL;
  }
  leme_option_store(prep->option, (char *)cfg + prep->group->base,
                    &prep->parsed);
  if (prep->group->fields != 0) {
    *(uint32_t *)((char *)cfg + prep->group->fields) |= prep->option->field;
  }
  refresh_setting(server, prep->option->effect);

  enum leme_control_code ov_code = leme_config_store_add_override(
      server->config_store, NULL, (const char *const *)prep->path,
      prep->path_count, prep->value, error);
  if (ov_code != LEME_CONTROL_OK) {
    return ov_code;
  }

  leme_public_server_invalidate(server);
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
leme_config_live_set(struct leme_server *server, const char *const *path,
                     size_t path_count, const struct leme_public_value *value,
                     bool *applied, struct leme_control_error *error) {
  *applied = false;
  struct leme_config_prepared_set *prep = NULL;
  enum leme_control_code code =
      prepare_set(server, path, path_count, value, &prep, error);
  if (code == LEME_CONTROL_OK)
    code = apply(server, prep, applied, error);
  cleanup_prepared_set(prep);
  return code;
}
