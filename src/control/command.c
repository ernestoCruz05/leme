#include "control/command.h"

#include "control/action-internal.h"
#include "control/control.h"
#include "control/error.h"
#include "control/limits.h"
#include "control/memory.h"
#include "core/command.h"
#include "public/identity.h"
#include "public/model-internal.h"
#include "public/model.h"
#include "public/schema.h"
#include "public/value.h"
#include "workspace/layout.h"
#include "workspace/public.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct leme_public_text dup_text(struct leme_public_budget *account,
                                        struct leme_public_text src) {
  if (src.length == 0 || src.data == NULL) {
    return (struct leme_public_text){.data = "", .length = 0};
  }
  char *buf = leme_control_alloc(account, src.length + 1);
  if (buf == NULL) {
    return (struct leme_public_text){0};
  }
  memcpy(buf, src.data, src.length);
  buf[src.length] = '\0';
  return (struct leme_public_text){.data = buf, .length = src.length};
}

static enum leme_control_code
set_preflight_error(struct leme_control_error *error,
                    enum leme_control_code code, const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%.*s",
                   (int)sizeof(error->message) - 1, msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

static struct leme_control_intent *
alloc_intents(struct leme_public_budget *account, size_t count) {
  struct leme_control_intent *intents =
      leme_control_alloc(account, count * sizeof(struct leme_control_intent));
  if (intents != NULL) {
    memset(intents, 0, count * sizeof(struct leme_control_intent));
  }
  return intents;
}

static const char *layout_kind_name(enum leme_layout_kind kind) {
  switch (kind) {
  case LEME_LAYOUT_DWINDLE:
    return "dwindle";
  case LEME_LAYOUT_MASTER_STACK:
    return "master_stack";
  case LEME_LAYOUT_ACCORDION:
    return "accordion";
  }
  return "dwindle";
}

static void set_target_id(struct leme_public_budget *account,
                          struct leme_control_intent *intent,
                          enum leme_public_entity kind,
                          struct leme_public_id id, uint16_t tag_number,
                          struct leme_public_text id_text) {
  intent->has_target = true;
  intent->target.kind = kind;
  intent->target.id = id;
  intent->target.tag_number = tag_number;
  intent->effective_id = dup_text(account, id_text);
  intent->requested_targets =
      leme_control_alloc(account, sizeof(struct leme_public_text));
  if (intent->requested_targets != NULL) {
    intent->requested_targets[0] = dup_text(account, id_text);
    intent->requested_count = 1;
  }
}

static void set_destination_id(struct leme_control_intent *intent,
                               enum leme_public_entity kind,
                               struct leme_public_id id, uint16_t tag_number) {
  intent->has_destination = true;
  intent->destination.kind = kind;
  intent->destination.id = id;
  intent->destination.tag_number = tag_number;
}

static const uint16_t occupied_slot_limit = 64;

static uint16_t occupied_neighbour(uint64_t occupied, uint16_t active,
                                   uint16_t max_tags, bool forward) {
  if (active == 0 || active > max_tags || max_tags > occupied_slot_limit) {
    return active;
  }
  uint16_t slot = active;
  for (uint16_t step = 1; step < max_tags; ++step) {
    slot = forward ? (slot == max_tags ? 1 : (uint16_t)(slot + 1))
                   : (slot == 1 ? max_tags : (uint16_t)(slot - 1));
    if (((occupied >> (slot - 1)) & 1u) != 0) {
      return slot;
    }
  }
  return active;
}

static const char *direction_to_string(enum leme_direction dir) {
  switch (dir) {
  case LEME_DIRECTION_LEFT:
    return "left";
  case LEME_DIRECTION_RIGHT:
    return "right";
  case LEME_DIRECTION_UP:
    return "up";
  case LEME_DIRECTION_DOWN:
    return "down";
  }
  return "left";
}

static const char *edge_to_string(enum leme_resize_edge edge) {
  switch (edge) {
  case LEME_RESIZE_LEFT:
    return "left";
  case LEME_RESIZE_RIGHT:
    return "right";
  case LEME_RESIZE_UP:
    return "up";
  case LEME_RESIZE_DOWN:
    return "down";
  }
  return "left";
}

void leme_control_intent_batch_destroy(
    struct leme_public_budget *account,
    struct leme_control_intent_batch *batch) {
  (void)account;
  if (batch == NULL) {
    return;
  }
  if (batch->intents != NULL) {
    leme_control_intents_destroy(batch->intents, batch->count);
    batch->intents = NULL;
    batch->count = 0;
  }
  if (batch->builder != NULL) {
    leme_public_builder_destroy(batch->builder);
    batch->builder = NULL;
  }
}

static enum leme_control_code make_cmd_args(struct leme_public_builder *b,
                                            const char *name,
                                            const char *arg1_str,
                                            int64_t arg1_int, bool has_arg1_int,
                                            int64_t arg2_int, bool has_arg2_int,
                                            struct leme_public_value **out) {
  size_t count = 1;
  if (arg1_str != NULL || has_arg1_int) {
    count++;
  }
  if (has_arg2_int) {
    count++;
  }

  struct leme_public_value *arr = NULL;
  if (leme_public_array(b, count, &arr) != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  struct leme_public_value *v0 = NULL;
  if (leme_public_string(b, (struct leme_public_text){name, strlen(name)},
                         false, &v0) != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  if (leme_public_array_set(b, arr, 0, v0) != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  size_t idx = 1;
  if (arg1_str != NULL) {
    struct leme_public_value *v1 = NULL;
    if (leme_public_string(
            b, (struct leme_public_text){arg1_str, strlen(arg1_str)}, false,
            &v1) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    if (leme_public_array_set(b, arr, idx++, v1) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  } else if (has_arg1_int) {
    struct leme_public_value *v1 = NULL;
    if (leme_public_integer(b, arg1_int, &v1) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    if (leme_public_array_set(b, arr, idx++, v1) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }

  if (has_arg2_int) {
    struct leme_public_value *v2 = NULL;
    if (leme_public_integer(b, arg2_int, &v2) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    if (leme_public_array_set(b, arr, idx++, v2) != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }

  *out = arr;
  return LEME_CONTROL_OK;
}

enum leme_control_code leme_control_command_lower(
    struct leme_control_context *context,
    const struct leme_public_snapshot *snapshot, struct leme_public_text name,
    const struct leme_public_value *argv, struct leme_control_intent_batch *out,
    struct leme_control_error *error) {
  if (context == NULL || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  out->intents = NULL;
  out->count = 0;
  out->builder = NULL;

  if (name.data == NULL || name.length == 0) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "empty command name");
  }
  if (memchr(name.data, ' ', name.length) != NULL) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "command name cannot contain spaces");
  }
  if (memchr(name.data, '\0', name.length) != NULL) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "command name contains embedded NUL");
  }

  if (argv == NULL || leme_public_kind(argv) != LEME_PUBLIC_ARRAY) {
    return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                               "argv must be array");
  }

  size_t argc = leme_public_length(argv);
  for (size_t i = 0; i < argc; ++i) {
    const struct leme_public_value *elem = leme_public_at(argv, i);
    if (elem == NULL || leme_public_kind(elem) != LEME_PUBLIC_STRING) {
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "argv element must be string");
    }
    struct leme_public_text t = {0};
    leme_public_as_text(elem, &t);
    if (t.data != NULL && memchr(t.data, '\0', t.length) != NULL) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "argv element contains embedded NUL");
    }
  }

  char **params = (char **)malloc((argc + 2) * sizeof(char *));
  if (params == NULL) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  params[0] = strndup(name.data, name.length);
  for (size_t i = 0; i < argc; ++i) {
    struct leme_public_text t = {0};
    leme_public_as_text(leme_public_at(argv, i), &t);
    params[i + 1] = t.data != NULL ? strndup(t.data, t.length) : strdup("");
  }
  params[argc + 1] = NULL;

  struct leme_command cmd = {0};
  char *parse_err = NULL;
  bool parse_ok = leme_command_parse(&cmd, params, argc + 1, &parse_err);
  for (size_t i = 0; i <= argc; ++i) {
    free(params[i]);
  }
  free((void *)params);

  if (!parse_ok) {
    char err_buf[256];
    (void)snprintf(err_buf, sizeof(err_buf), "%s",
                   parse_err != NULL ? parse_err : "invalid command syntax");
    free(parse_err);
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT, err_buf);
  }
  free(parse_err);

  if (cmd.type == LEME_COMMAND_SPAWN || cmd.type == LEME_COMMAND_QUIT ||
      cmd.type == LEME_COMMAND_SWITCH_VT) {
    leme_command_finish(&cmd);
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "command refused");
  }

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  struct leme_public_model *model = leme_control_context_model(context);

  if (snapshot != NULL) {
    const struct leme_public_value *session_val =
        leme_public_snapshot_root(snapshot, LEME_PUBLIC_SESSION);
    if (session_val != NULL &&
        leme_public_kind(session_val) == LEME_PUBLIC_OBJECT) {
      const struct leme_public_value *locked_val =
          leme_public_get(session_val, LEME_PUBLIC_TEXT("locked"));
      bool locked = false;
      if (locked_val != NULL &&
          leme_public_as_bool(locked_val, &locked) == LEME_PUBLIC_OK &&
          locked) {
        if (cmd.text != NULL) {
          free(cmd.text);
        }
        return set_preflight_error(error, LEME_CONTROL_SESSION_LOCKED,
                                   "session is locked");
      }
    }
  }

  struct leme_public_id focused_view_id = {0};
  struct leme_public_text focused_view_id_text = {0};
  bool has_focused_view = false;
  bool focused_view_floating = false;
  bool focused_view_fullscreen = false;
  bool focused_view_sticky = false;

  struct leme_public_id focused_output_id = {0};
  struct leme_public_text focused_output_id_text = {0};
  bool has_focused_output = false;

  uint16_t active_tag_slot = 1;
  struct leme_public_id active_tag_id = {0};
  struct leme_public_text active_tag_id_text = {0};
  bool has_active_tag = false;
  uint16_t max_tags = 1;
  uint16_t last_materialized = 1;
  uint64_t occupied_slots = 0;

  const struct leme_public_value *session_root = NULL;
  const struct leme_public_value *views_root = NULL;
  const struct leme_public_value *outputs_root = NULL;
  const struct leme_public_value *tags_root = NULL;

  if (snapshot != NULL) {
    session_root = leme_public_snapshot_root(snapshot, LEME_PUBLIC_SESSION);
    views_root = leme_public_snapshot_root(snapshot, LEME_PUBLIC_VIEWS);
    outputs_root = leme_public_snapshot_root(snapshot, LEME_PUBLIC_OUTPUTS);
    tags_root = leme_public_snapshot_root(snapshot, LEME_PUBLIC_TAGS);
  }

  if (session_root != NULL &&
      leme_public_kind(session_root) == LEME_PUBLIC_OBJECT) {
    const struct leme_public_value *fv =
        leme_public_get(session_root, LEME_PUBLIC_TEXT("focused_view"));
    if (fv != NULL && leme_public_kind(fv) == LEME_PUBLIC_OBJECT) {
      const struct leme_public_value *id_v =
          leme_public_get(fv, LEME_PUBLIC_TEXT("id"));
      if (id_v != NULL && leme_public_kind(id_v) == LEME_PUBLIC_STRING) {
        leme_public_as_text(id_v, &focused_view_id_text);
        uint16_t dummy = 0;
        if (leme_public_parse_id(model, LEME_PUBLIC_VIEW, focused_view_id_text,
                                 &focused_view_id, &dummy)) {
          has_focused_view = true;
        }
      }
    }

    const struct leme_public_value *fo =
        leme_public_get(session_root, LEME_PUBLIC_TEXT("focused_output"));
    if (fo != NULL && leme_public_kind(fo) == LEME_PUBLIC_OBJECT) {
      const struct leme_public_value *id_o =
          leme_public_get(fo, LEME_PUBLIC_TEXT("id"));
      if (id_o != NULL && leme_public_kind(id_o) == LEME_PUBLIC_STRING) {
        leme_public_as_text(id_o, &focused_output_id_text);
        uint16_t dummy = 0;
        if (leme_public_parse_id(model, LEME_PUBLIC_OUTPUT,
                                 focused_output_id_text, &focused_output_id,
                                 &dummy)) {
          has_focused_output = true;
        }
      }
    }
  }

  if (!has_focused_view && views_root != NULL &&
      leme_public_kind(views_root) == LEME_PUBLIC_ARRAY) {
    size_t v_count = leme_public_length(views_root);
    for (size_t i = 0; i < v_count; ++i) {
      const struct leme_public_value *v = leme_public_at(views_root, i);
      const struct leme_public_value *foc =
          leme_public_get(v, LEME_PUBLIC_TEXT("focused"));
      bool is_foc = false;
      if (foc != NULL && leme_public_as_bool(foc, &is_foc) == LEME_PUBLIC_OK &&
          is_foc) {
        const struct leme_public_value *id_v =
            leme_public_get(v, LEME_PUBLIC_TEXT("id"));
        if (id_v != NULL && leme_public_kind(id_v) == LEME_PUBLIC_STRING) {
          leme_public_as_text(id_v, &focused_view_id_text);
          uint16_t dummy = 0;
          if (leme_public_parse_id(model, LEME_PUBLIC_VIEW,
                                   focused_view_id_text, &focused_view_id,
                                   &dummy)) {
            has_focused_view = true;
            break;
          }
        }
      }
    }
  }

  if (has_focused_view && views_root != NULL &&
      leme_public_kind(views_root) == LEME_PUBLIC_ARRAY) {
    size_t v_count = leme_public_length(views_root);
    for (size_t i = 0; i < v_count; ++i) {
      const struct leme_public_value *v = leme_public_at(views_root, i);
      const struct leme_public_value *id_v =
          leme_public_get(v, LEME_PUBLIC_TEXT("id"));
      struct leme_public_text cur_t = {0};
      if (id_v != NULL && leme_public_as_text(id_v, &cur_t) == LEME_PUBLIC_OK) {
        if (focused_view_id_text.data != NULL && cur_t.data != NULL &&
            cur_t.length == focused_view_id_text.length &&
            memcmp(cur_t.data, focused_view_id_text.data, cur_t.length) == 0) {
          const struct leme_public_value *fl =
              leme_public_get(v, LEME_PUBLIC_TEXT("floating"));
          if (fl != NULL) {
            leme_public_as_bool(fl, &focused_view_floating);
          }
          const struct leme_public_value *fs =
              leme_public_get(v, LEME_PUBLIC_TEXT("fullscreen"));
          if (fs != NULL) {
            leme_public_as_bool(fs, &focused_view_fullscreen);
          }
          const struct leme_public_value *own =
              leme_public_get(v, LEME_PUBLIC_TEXT("owner"));
          if (own != NULL && leme_public_kind(own) == LEME_PUBLIC_OBJECT) {
            const struct leme_public_value *k =
                leme_public_get(own, LEME_PUBLIC_TEXT("kind"));
            struct leme_public_text kt = {0};
            if (k != NULL && leme_public_as_text(k, &kt) == LEME_PUBLIC_OK) {
              if (kt.length == 6 && memcmp(kt.data, "sticky", 6) == 0) {
                focused_view_sticky = true;
              }
            }
          }
          break;
        }
      }
    }
  }

  if (!has_focused_output && outputs_root != NULL &&
      leme_public_kind(outputs_root) == LEME_PUBLIC_ARRAY) {
    size_t o_count = leme_public_length(outputs_root);
    for (size_t i = 0; i < o_count; ++i) {
      const struct leme_public_value *o = leme_public_at(outputs_root, i);
      const struct leme_public_value *foc =
          leme_public_get(o, LEME_PUBLIC_TEXT("focused"));
      bool is_foc = false;
      if (foc != NULL && leme_public_as_bool(foc, &is_foc) == LEME_PUBLIC_OK &&
          is_foc) {
        const struct leme_public_value *id_o =
            leme_public_get(o, LEME_PUBLIC_TEXT("id"));
        if (id_o != NULL && leme_public_kind(id_o) == LEME_PUBLIC_STRING) {
          leme_public_as_text(id_o, &focused_output_id_text);
          uint16_t dummy = 0;
          if (leme_public_parse_id(model, LEME_PUBLIC_OUTPUT,
                                   focused_output_id_text, &focused_output_id,
                                   &dummy)) {
            has_focused_output = true;
            break;
          }
        }
      }
    }
    if (!has_focused_output && o_count > 0) {
      const struct leme_public_value *o = leme_public_at(outputs_root, 0);
      const struct leme_public_value *id_o =
          leme_public_get(o, LEME_PUBLIC_TEXT("id"));
      if (id_o != NULL && leme_public_kind(id_o) == LEME_PUBLIC_STRING) {
        leme_public_as_text(id_o, &focused_output_id_text);
        uint16_t dummy = 0;
        if (leme_public_parse_id(model, LEME_PUBLIC_OUTPUT,
                                 focused_output_id_text, &focused_output_id,
                                 &dummy)) {
          has_focused_output = true;
        }
      }
    }
  }

  if (has_focused_output && tags_root != NULL &&
      leme_public_kind(tags_root) == LEME_PUBLIC_ARRAY) {
    size_t t_count = leme_public_length(tags_root);
    for (size_t i = 0; i < t_count; ++i) {
      const struct leme_public_value *tag = leme_public_at(tags_root, i);
      const struct leme_public_value *out_ref =
          leme_public_get(tag, LEME_PUBLIC_TEXT("output"));
      if (out_ref != NULL && leme_public_kind(out_ref) == LEME_PUBLIC_OBJECT) {
        const struct leme_public_value *out_id =
            leme_public_get(out_ref, LEME_PUBLIC_TEXT("id"));
        struct leme_public_text out_id_t = {0};
        if (out_id != NULL &&
            leme_public_as_text(out_id, &out_id_t) == LEME_PUBLIC_OK) {
          if (focused_output_id_text.data != NULL && out_id_t.data != NULL &&
              out_id_t.length == focused_output_id_text.length &&
              memcmp(out_id_t.data, focused_output_id_text.data,
                     out_id_t.length) == 0) {
            int64_t num = 0;
            const struct leme_public_value *n_val =
                leme_public_get(tag, LEME_PUBLIC_TEXT("number"));
            if (n_val != NULL &&
                leme_public_as_integer(n_val, &num) == LEME_PUBLIC_OK) {
              uint16_t s = (uint16_t)num;
              if (s > max_tags) {
                max_tags = s;
              }
              int64_t views = 0;
              const struct leme_public_value *v_val =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("view_count"));
              if (s >= 1 && s <= occupied_slot_limit && v_val != NULL &&
                  leme_public_as_integer(v_val, &views) == LEME_PUBLIC_OK &&
                  views > 0) {
                occupied_slots |= UINT64_C(1) << (s - 1);
              }
              bool mat = false;
              const struct leme_public_value *m_val =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("materialized"));
              if (m_val != NULL &&
                  leme_public_as_bool(m_val, &mat) == LEME_PUBLIC_OK && mat) {
                if (s > last_materialized) {
                  last_materialized = s;
                }
              }
              bool act = false;
              const struct leme_public_value *a_val =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("active"));
              if (a_val != NULL &&
                  leme_public_as_bool(a_val, &act) == LEME_PUBLIC_OK && act) {
                active_tag_slot = s;
                const struct leme_public_value *t_id =
                    leme_public_get(tag, LEME_PUBLIC_TEXT("id"));
                if (t_id != NULL) {
                  leme_public_as_text(t_id, &active_tag_id_text);
                  uint16_t dummy = 0;
                  if (leme_public_parse_id(model, LEME_PUBLIC_TAG,
                                           active_tag_id_text, &active_tag_id,
                                           &dummy)) {
                    has_active_tag = true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  uint16_t wrap_id = last_materialized < max_tags
                         ? (uint16_t)(last_materialized + 1)
                         : max_tags;
  uint16_t next_slot =
      active_tag_slot == max_tags ? 1 : (uint16_t)(active_tag_slot + 1);
  uint16_t prev_slot =
      active_tag_slot == 1 ? wrap_id : (uint16_t)(active_tag_slot - 1);

  struct leme_public_builder *b = NULL;
  if (leme_public_builder_create_budget(account, limits->response_bytes, &b) !=
      LEME_PUBLIC_OK) {
    if (cmd.text != NULL) {
      free(cmd.text);
    }
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }

  switch (cmd.type) {
  case LEME_COMMAND_CLOSE_VIEW: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_CLOSE;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_TOGGLE_FLOATING: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_SET_FLOATING;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *val = NULL;
    leme_public_boolean(b, !focused_view_floating, &val);
    intents[0].args = val;
    const struct leme_public_value *roots[1] = {val};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_TOGGLE_FULLSCREEN: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_SET_FULLSCREEN;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *val = NULL;
    leme_public_boolean(b, !focused_view_fullscreen, &val);
    intents[0].args = val;
    const struct leme_public_value *roots[1] = {val};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_TOGGLE_STICKY: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_SET_STICKY;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *val = NULL;
    leme_public_boolean(b, !focused_view_sticky, &val);
    intents[0].args = val;
    const struct leme_public_value *roots[1] = {val};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_RESIZE: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_RESIZE;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *arr = NULL;
    leme_public_array(b, 2, &arr);
    const char *e_str = edge_to_string(cmd.edge);
    struct leme_public_value *e_val = NULL;
    struct leme_public_value *a_val = NULL;
    leme_public_string(b, (struct leme_public_text){e_str, strlen(e_str)},
                       false, &e_val);
    leme_public_number(b, (double)cmd.amount, &a_val);
    leme_public_array_set(b, arr, 0, e_val);
    leme_public_array_set(b, arr, 1, a_val);
    intents[0].args = arr;
    const struct leme_public_value *roots[1] = {arr};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_FOCUS_TAG:
  case LEME_COMMAND_FOCUS_NEXT_TAG:
  case LEME_COMMAND_FOCUS_PREVIOUS_TAG: {
    if (!has_focused_output) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused output");
    }
    uint16_t target_slot = cmd.tag_id;
    if (cmd.type != LEME_COMMAND_FOCUS_TAG) {
      const bool forward = cmd.type == LEME_COMMAND_FOCUS_NEXT_TAG;
      if (cmd.occupied) {
        target_slot = occupied_neighbour(occupied_slots, active_tag_slot,
                                         max_tags, forward);
      } else {
        target_slot = forward ? next_slot : prev_slot;
      }
    }
    struct leme_public_id target_tag_id = {0};
    struct leme_public_text target_tag_id_text = {0};
    bool found_tag = false;
    if (tags_root != NULL && leme_public_kind(tags_root) == LEME_PUBLIC_ARRAY) {
      size_t t_count = leme_public_length(tags_root);
      for (size_t i = 0; i < t_count; ++i) {
        const struct leme_public_value *tag = leme_public_at(tags_root, i);
        const struct leme_public_value *out_ref =
            leme_public_get(tag, LEME_PUBLIC_TEXT("output"));
        if (out_ref != NULL) {
          const struct leme_public_value *out_id =
              leme_public_get(out_ref, LEME_PUBLIC_TEXT("id"));
          struct leme_public_text o_t = {0};
          if (out_id != NULL &&
              leme_public_as_text(out_id, &o_t) == LEME_PUBLIC_OK) {
            if (focused_output_id_text.data != NULL && o_t.data != NULL &&
                o_t.length == focused_output_id_text.length &&
                memcmp(o_t.data, focused_output_id_text.data, o_t.length) ==
                    0) {
              int64_t n = 0;
              const struct leme_public_value *nv =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("number"));
              if (nv != NULL &&
                  leme_public_as_integer(nv, &n) == LEME_PUBLIC_OK &&
                  n == target_slot) {
                const struct leme_public_value *t_id =
                    leme_public_get(tag, LEME_PUBLIC_TEXT("id"));
                if (t_id != NULL) {
                  leme_public_as_text(t_id, &target_tag_id_text);
                  uint16_t dummy = 0;
                  if (leme_public_parse_id(model, LEME_PUBLIC_TAG,
                                           target_tag_id_text, &target_tag_id,
                                           &dummy)) {
                    found_tag = true;
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
    if (!found_tag) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "tag not found");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_FOCUS_TAG;
    set_target_id(account, &intents[0], LEME_PUBLIC_TAG, target_tag_id,
                  target_slot, target_tag_id_text);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_FOCUS_OUTPUT: {
    if (!cmd.has_direction) {
      struct leme_public_id target_out_id = {0};
      struct leme_public_text target_out_id_text = {0};
      bool found_out = false;
      if (outputs_root != NULL &&
          leme_public_kind(outputs_root) == LEME_PUBLIC_ARRAY) {
        size_t o_count = leme_public_length(outputs_root);
        for (size_t i = 0; i < o_count; ++i) {
          const struct leme_public_value *o = leme_public_at(outputs_root, i);
          const struct leme_public_value *nm =
              leme_public_get(o, LEME_PUBLIC_TEXT("name"));
          struct leme_public_text nm_t = {0};
          if (nm != NULL && leme_public_as_text(nm, &nm_t) == LEME_PUBLIC_OK &&
              cmd.text != NULL) {
            if (nm_t.length == strlen(cmd.text) &&
                memcmp(nm_t.data, cmd.text, nm_t.length) == 0) {
              const struct leme_public_value *id_o =
                  leme_public_get(o, LEME_PUBLIC_TEXT("id"));
              if (id_o != NULL) {
                leme_public_as_text(id_o, &target_out_id_text);
                uint16_t dummy = 0;
                if (leme_public_parse_id(model, LEME_PUBLIC_OUTPUT,
                                         target_out_id_text, &target_out_id,
                                         &dummy)) {
                  found_out = true;
                  break;
                }
              }
            }
          }
        }
      }
      free(cmd.text);
      cmd.text = NULL;
      if (!found_out) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                   "output not found");
      }
      struct leme_control_intent *intents = alloc_intents(account, 1);
      if (intents == NULL) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      intents[0].opcode = LEME_CONTROL_OP_FOCUS_OUTPUT;
      set_target_id(account, &intents[0], LEME_PUBLIC_OUTPUT, target_out_id, 0,
                    target_out_id_text);
      out->intents = intents;
      out->count = 1;
      out->builder = b;
      return LEME_CONTROL_OK;
    } else {
      struct leme_control_intent *intents = alloc_intents(account, 1);
      if (intents == NULL) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      intents[0].opcode = LEME_CONTROL_OP_COMMAND;
      if (has_focused_output) {
        set_target_id(account, &intents[0], LEME_PUBLIC_OUTPUT,
                      focused_output_id, 0, focused_output_id_text);
      }
      struct leme_public_value *c_args = NULL;
      make_cmd_args(b, "focus_output", direction_to_string(cmd.direction), 0,
                    false, 0, false, &c_args);
      intents[0].args = c_args;
      const struct leme_public_value *roots[1] = {c_args};
      leme_public_builder_seal(b, roots, 1);
      out->intents = intents;
      out->count = 1;
      out->builder = b;
      return LEME_CONTROL_OK;
    }
  }

  case LEME_COMMAND_MOVE_VIEW_TO_TAG: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    uint16_t target_slot =
        cmd.has_direction
            ? (cmd.direction == LEME_DIRECTION_LEFT ? prev_slot : next_slot)
            : cmd.tag_id;
    struct leme_public_id target_tag_id = {0};
    struct leme_public_text target_tag_id_text = {0};
    bool found_tag = false;
    if (tags_root != NULL && leme_public_kind(tags_root) == LEME_PUBLIC_ARRAY) {
      size_t t_count = leme_public_length(tags_root);
      for (size_t i = 0; i < t_count; ++i) {
        const struct leme_public_value *tag = leme_public_at(tags_root, i);
        const struct leme_public_value *out_ref =
            leme_public_get(tag, LEME_PUBLIC_TEXT("output"));
        if (out_ref != NULL) {
          const struct leme_public_value *out_id =
              leme_public_get(out_ref, LEME_PUBLIC_TEXT("id"));
          struct leme_public_text o_t = {0};
          if (out_id != NULL &&
              leme_public_as_text(out_id, &o_t) == LEME_PUBLIC_OK) {
            if (focused_output_id_text.data != NULL && o_t.data != NULL &&
                o_t.length == focused_output_id_text.length &&
                memcmp(o_t.data, focused_output_id_text.data, o_t.length) ==
                    0) {
              int64_t n = 0;
              const struct leme_public_value *nv =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("number"));
              if (nv != NULL &&
                  leme_public_as_integer(nv, &n) == LEME_PUBLIC_OK &&
                  n == target_slot) {
                const struct leme_public_value *t_id =
                    leme_public_get(tag, LEME_PUBLIC_TEXT("id"));
                if (t_id != NULL) {
                  leme_public_as_text(t_id, &target_tag_id_text);
                  uint16_t dummy = 0;
                  if (leme_public_parse_id(model, LEME_PUBLIC_TAG,
                                           target_tag_id_text, &target_tag_id,
                                           &dummy)) {
                    found_tag = true;
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
    if (!found_tag) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "destination tag not found");
    }

    size_t count = cmd.follow ? 3 : 1;
    struct leme_control_intent *intents = alloc_intents(account, count);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_MOVE_TO_TAG;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    set_destination_id(&intents[0], LEME_PUBLIC_TAG, target_tag_id,
                       target_slot);
    if (cmd.follow) {
      intents[1].opcode = LEME_CONTROL_OP_FOCUS_TAG;
      set_target_id(account, &intents[1], LEME_PUBLIC_TAG, target_tag_id,
                    target_slot, target_tag_id_text);
      intents[2].opcode = LEME_CONTROL_OP_FOCUS;
      set_target_id(account, &intents[2], LEME_PUBLIC_VIEW, focused_view_id, 0,
                    focused_view_id_text);
    }
    out->intents = intents;
    out->count = count;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT: {
    if (!has_focused_view) {
      if (cmd.text != NULL) {
        free(cmd.text);
      }
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    if (!cmd.has_direction) {
      struct leme_public_id target_out_id = {0};
      struct leme_public_text target_out_id_text = {0};
      bool found_out = false;
      if (outputs_root != NULL &&
          leme_public_kind(outputs_root) == LEME_PUBLIC_ARRAY) {
        size_t o_count = leme_public_length(outputs_root);
        for (size_t i = 0; i < o_count; ++i) {
          const struct leme_public_value *o = leme_public_at(outputs_root, i);
          const struct leme_public_value *nm =
              leme_public_get(o, LEME_PUBLIC_TEXT("name"));
          struct leme_public_text nm_t = {0};
          if (nm != NULL && leme_public_as_text(nm, &nm_t) == LEME_PUBLIC_OK &&
              cmd.text != NULL) {
            if (nm_t.length == strlen(cmd.text) &&
                memcmp(nm_t.data, cmd.text, nm_t.length) == 0) {
              const struct leme_public_value *id_o =
                  leme_public_get(o, LEME_PUBLIC_TEXT("id"));
              if (id_o != NULL) {
                leme_public_as_text(id_o, &target_out_id_text);
                uint16_t dummy = 0;
                if (leme_public_parse_id(model, LEME_PUBLIC_OUTPUT,
                                         target_out_id_text, &target_out_id,
                                         &dummy)) {
                  found_out = true;
                  break;
                }
              }
            }
          }
        }
      }
      free(cmd.text);
      cmd.text = NULL;
      if (!found_out) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                   "destination output not found");
      }
      size_t count = cmd.follow ? 3 : 1;
      struct leme_control_intent *intents = alloc_intents(account, count);
      if (intents == NULL) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      intents[0].opcode = LEME_CONTROL_OP_MOVE_TO_OUTPUT;
      set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                    focused_view_id_text);
      set_destination_id(&intents[0], LEME_PUBLIC_OUTPUT, target_out_id, 0);
      if (cmd.follow) {
        intents[1].opcode = LEME_CONTROL_OP_FOCUS_OUTPUT;
        set_target_id(account, &intents[1], LEME_PUBLIC_OUTPUT, target_out_id,
                      0, target_out_id_text);
        intents[2].opcode = LEME_CONTROL_OP_FOCUS;
        set_target_id(account, &intents[2], LEME_PUBLIC_VIEW, focused_view_id,
                      0, focused_view_id_text);
      }
      out->intents = intents;
      out->count = count;
      out->builder = b;
      return LEME_CONTROL_OK;
    } else {
      struct leme_control_intent *intents = alloc_intents(account, 1);
      if (intents == NULL) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      intents[0].opcode = LEME_CONTROL_OP_COMMAND;
      set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                    focused_view_id_text);
      struct leme_public_value *c_args = NULL;
      make_cmd_args(b, "move_view_to_output",
                    direction_to_string(cmd.direction), 0, false,
                    cmd.follow ? 1 : 0, true, &c_args);
      intents[0].args = c_args;
      const struct leme_public_value *roots[1] = {c_args};
      leme_public_builder_seal(b, roots, 1);
      out->intents = intents;
      out->count = 1;
      out->builder = b;
      return LEME_CONTROL_OK;
    }
  }

  case LEME_COMMAND_SET_LAYOUT: {
    if (!has_active_tag) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no active tag");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_SET_LAYOUT;
    set_target_id(account, &intents[0], LEME_PUBLIC_TAG, active_tag_id,
                  active_tag_slot, active_tag_id_text);
    const char *l_name = layout_kind_name(cmd.layout);
    struct leme_public_value *l_val = NULL;
    leme_public_string(b, (struct leme_public_text){l_name, strlen(l_name)},
                       false, &l_val);
    intents[0].args = l_val;
    const struct leme_public_value *roots[1] = {l_val};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_SET_MODE: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      free(cmd.text);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_SET_MODE;
    struct leme_public_value *m_val = NULL;
    leme_public_string(b, (struct leme_public_text){cmd.text, strlen(cmd.text)},
                       false, &m_val);
    free(cmd.text);
    cmd.text = NULL;
    intents[0].args = m_val;
    const struct leme_public_value *roots[1] = {m_val};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_RELOAD_CONFIG: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_RELOAD_CONFIG;
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_FOCUS_DIRECTION: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    if (has_focused_view) {
      set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                    focused_view_id_text);
    }
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "focus", direction_to_string(cmd.direction), 0, false, 0,
                  false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_FOCUS_PREVIOUS_VIEW: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    if (has_focused_view) {
      set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                    focused_view_id_text);
    }
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "focus_previous_view", NULL, 0, false, 0, false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_FOCUS_LAST_TAG: {
    if (!has_focused_output) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused output");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_OUTPUT, focused_output_id,
                  0, focused_output_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "focus_last_tag", NULL, 0, false, 0, false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_MOVE_DIRECTION: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "move", direction_to_string(cmd.direction), 0, false,
                  cmd.amount, true, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_SWITCH_LAYOUT: {
    if (!has_active_tag) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no active tag");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_TAG, active_tag_id,
                  active_tag_slot, active_tag_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "switch_layout", NULL, 0, false, 0, false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_REMOVE_EMPTY_TAG: {
    if (!has_focused_output) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused output");
    }
    struct leme_public_id target_tag_id = {0};
    struct leme_public_text target_tag_id_text = {0};
    bool found_tag = false;
    if (tags_root != NULL && leme_public_kind(tags_root) == LEME_PUBLIC_ARRAY) {
      size_t t_count = leme_public_length(tags_root);
      for (size_t i = 0; i < t_count; ++i) {
        const struct leme_public_value *tag = leme_public_at(tags_root, i);
        const struct leme_public_value *out_ref =
            leme_public_get(tag, LEME_PUBLIC_TEXT("output"));
        if (out_ref != NULL) {
          const struct leme_public_value *out_id =
              leme_public_get(out_ref, LEME_PUBLIC_TEXT("id"));
          struct leme_public_text o_t = {0};
          if (out_id != NULL &&
              leme_public_as_text(out_id, &o_t) == LEME_PUBLIC_OK) {
            if (focused_output_id_text.data != NULL && o_t.data != NULL &&
                o_t.length == focused_output_id_text.length &&
                memcmp(o_t.data, focused_output_id_text.data, o_t.length) ==
                    0) {
              int64_t n = 0;
              const struct leme_public_value *nv =
                  leme_public_get(tag, LEME_PUBLIC_TEXT("number"));
              if (nv != NULL &&
                  leme_public_as_integer(nv, &n) == LEME_PUBLIC_OK &&
                  n == cmd.tag_id) {
                const struct leme_public_value *t_id =
                    leme_public_get(tag, LEME_PUBLIC_TEXT("id"));
                if (t_id != NULL) {
                  leme_public_as_text(t_id, &target_tag_id_text);
                  uint16_t dummy = 0;
                  if (leme_public_parse_id(model, LEME_PUBLIC_TAG,
                                           target_tag_id_text, &target_tag_id,
                                           &dummy)) {
                    found_tag = true;
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }
    if (!found_tag) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "tag not found");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_TAG, target_tag_id,
                  cmd.tag_id, target_tag_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "remove_empty_tag", NULL, cmd.tag_id, true, 0, false,
                  &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "cycle_keyboard_layout", NULL, 0, false, 0, false,
                  &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_TOGGLE_SHORTCUTS_INHIBIT: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "toggle_shortcuts_inhibit", NULL, 0, false, 0, false,
                  &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_SCRATCHPAD_SEND: {
    if (!has_focused_view) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_VIEW, focused_view_id, 0,
                  focused_view_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "scratchpad_send", NULL, 0, false, 0, false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_SCRATCHPAD_TOGGLE: {
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      if (cmd.text != NULL) {
        free(cmd.text);
      }
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    if (has_focused_output) {
      set_target_id(account, &intents[0], LEME_PUBLIC_OUTPUT, focused_output_id,
                    0, focused_output_id_text);
    }
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "scratchpad_toggle", cmd.text != NULL ? cmd.text : "", 0,
                  false, 0, false, &c_args);
    if (cmd.text != NULL) {
      free(cmd.text);
      cmd.text = NULL;
    }
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  case LEME_COMMAND_SCRATCHPAD_RETRIEVE: {
    if (!has_focused_output) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused output");
    }
    struct leme_control_intent *intents = alloc_intents(account, 1);
    if (intents == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    intents[0].opcode = LEME_CONTROL_OP_COMMAND;
    set_target_id(account, &intents[0], LEME_PUBLIC_OUTPUT, focused_output_id,
                  0, focused_output_id_text);
    struct leme_public_value *c_args = NULL;
    make_cmd_args(b, "scratchpad_retrieve", NULL, 0, false, 0, false, &c_args);
    intents[0].args = c_args;
    const struct leme_public_value *roots[1] = {c_args};
    leme_public_builder_seal(b, roots, 1);
    out->intents = intents;
    out->count = 1;
    out->builder = b;
    return LEME_CONTROL_OK;
  }

  default:
    if (cmd.text != NULL) {
      free(cmd.text);
    }
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported command");
  }
}
