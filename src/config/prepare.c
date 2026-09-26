#include "config/prepare.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/live-internal.h"
#include "config/live.h"
#include "control/memory.h"
#include "core/server.h"
#include "core/session_environment.h"
#include "input/input.h"
#include "input/public.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "public/server.h"
#include "render/render.h"
#include "shell/scratchpad.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

struct leme_config_reload {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  struct leme_config *next;
  struct leme_config *effective;
  struct wlr_xcursor_manager *cursor_manager;
  struct leme_tags_resize *resizes;
  size_t resize_count;
};

static bool leme_config_same_text(const char *first, const char *second) {
  if (first == NULL || second == NULL) {
    return first == second;
  }
  return strcmp(first, second) == 0;
}

static bool pointer_settings_equal(const struct leme_pointer_settings *a,
                                   const struct leme_pointer_settings *b) {
  if (a->fields != b->fields) {
    return false;
  }
  if ((a->fields & LEME_POINTER_PROFILE) && a->profile != b->profile) {
    return false;
  }
  if ((a->fields & LEME_POINTER_SPEED) && fabs(a->speed - b->speed) > 1e-9) {
    return false;
  }
  if ((a->fields & LEME_POINTER_NATURAL_SCROLL) &&
      a->natural_scroll != b->natural_scroll) {
    return false;
  }
  if ((a->fields & LEME_POINTER_LEFT_HANDED) &&
      a->left_handed != b->left_handed) {
    return false;
  }
  if ((a->fields & LEME_POINTER_TAP) && a->tap != b->tap) {
    return false;
  }
  return true;
}

static bool pointer_rules_equal(const struct leme_config *a,
                                const struct leme_config *b) {
  if (a->pointer_rule_count != b->pointer_rule_count) {
    return false;
  }
  for (size_t i = 0; i < a->pointer_rule_count; ++i) {
    if (!leme_config_same_text(a->pointer_rules[i].name,
                               b->pointer_rules[i].name) ||
        !pointer_settings_equal(&a->pointer_rules[i].settings,
                                &b->pointer_rules[i].settings)) {
      return false;
    }
  }
  return true;
}

static bool leme_pointers_have_delta(const struct leme_server *server,
                                     const struct leme_config *next) {
  if (server == NULL || server->pointers.next == NULL ||
      wl_list_empty(&server->pointers) || server->config == NULL) {
    return false;
  }
  if (!pointer_settings_equal(&server->config->pointer_defaults,
                              &next->pointer_defaults)) {
    return true;
  }
  return !pointer_rules_equal(server->config, next);
}

static bool leme_keyboards_have_delta(const struct leme_server *server,
                                      const struct leme_config *next) {
  if (server == NULL || server->keyboards.next == NULL ||
      wl_list_empty(&server->keyboards)) {
    return false;
  }
  if (server->config == NULL) {
    return next->keyboard_layout_count > 0;
  }
  if (server->config->keyboard_layout_count != next->keyboard_layout_count) {
    return true;
  }
  for (size_t i = 0; i < next->keyboard_layout_count; ++i) {
    const struct leme_keyboard_layout *a = &next->keyboard_layouts[i];
    const struct leme_keyboard_layout *b = &server->config->keyboard_layouts[i];
    if (!leme_config_same_text(a->name, b->name) ||
        !leme_config_same_text(a->variant, b->variant)) {
      return true;
    }
  }
  return false;
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

enum leme_control_code
leme_config_reload_prepare(struct leme_server *server, struct leme_config *next,
                           struct leme_public_budget *account,
                           struct leme_config_reload **out,
                           struct leme_control_error *error) {
  if (server == NULL || next == NULL || out == NULL) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid reload arguments");
  }
  *out = NULL;

  char *val_err = NULL;
  if (!leme_config_validate(next, &val_err)) {
    char msg[256];
    (void)snprintf(msg, sizeof(msg), "%s",
                   val_err != NULL ? val_err : "invalid configuration");
    free(val_err);
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT, msg);
  }

  if (leme_output_has_hardware_delta(server, next)) {
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported live output configuration change");
  }

  if (leme_keyboards_have_delta(server, next)) {
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported live keyboard keymap change");
  }

  if (leme_pointers_have_delta(server, next)) {
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported live pointer configuration change");
  }

  struct xkb_keymap *km = leme_input_compile_keymap(next);
  if (km == NULL) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid keyboard layout configuration");
  }
  xkb_keymap_unref(km);

  struct leme_config_reload *plan = NULL;
  if (account != NULL) {
    plan = leme_control_alloc(account, sizeof(*plan));
  } else {
    plan = calloc(1, sizeof(*plan));
  }
  if (plan == NULL) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(plan, 0, sizeof(*plan));
  plan->opcode = LEME_CONTROL_OP_RELOAD_CONFIG;
  plan->account = account;
  plan->next = next;

  size_t output_count = 0;
  if (server->outputs.next != NULL) {
    struct leme_output *output;
    wl_list_for_each(output, &server->outputs, link) { output_count++; }
  }
  if (output_count > 0) {
    if (output_count > SIZE_MAX / sizeof(struct leme_tags_resize)) {
      plan->next = NULL;
      leme_config_reload_discard(&plan);
      return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                                 "too many outputs");
    }
    struct leme_tags_resize *resizes = calloc(output_count, sizeof(*resizes));
    if (resizes == NULL) {
      plan->next = NULL;
      leme_config_reload_discard(&plan);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    size_t index = 0;
    struct leme_output *output;
    wl_list_for_each(output, &server->outputs, link) {
      if (!leme_tags_can_set_max(leme_output_tags(output), next->max_tags)) {
        for (size_t k = 0; k < index; ++k) {
          leme_tags_discard_set_max(&resizes[k]);
        }
        free(resizes);
        plan->next = NULL;
        leme_config_reload_discard(&plan);
        char msg[256];
        (void)snprintf(msg, sizeof(msg),
                       "maximum is below a materialized tag on %s",
                       output->wlr_output->name);
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT, msg);
      }
      if (!leme_tags_prepare_set_max(leme_output_tags(output), next->max_tags,
                                     &resizes[index])) {
        for (size_t k = 0; k < index; ++k) {
          leme_tags_discard_set_max(&resizes[k]);
        }
        free(resizes);
        plan->next = NULL;
        leme_config_reload_discard(&plan);
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "failed to prepare tag table on %s",
                       output->wlr_output->name);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY, msg);
      }
      index++;
    }
    plan->resizes = resizes;
    plan->resize_count = output_count;
  }

  if (server->desktop != NULL &&
      (server->config == NULL ||
       server->config->cursor.size != next->cursor.size ||
       !leme_config_same_text(server->config->cursor.theme,
                              next->cursor.theme))) {
    plan->cursor_manager = leme_desktop_prepare_cursor_config(
        server, next->cursor.theme, next->cursor.size);
    if (plan->cursor_manager == NULL) {
      plan->next = NULL;
      leme_config_reload_discard(&plan);
      return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                                 "cursor theme or size unavailable");
    }
  }

  if (server->config_store != NULL) {
    enum leme_control_code copy_code =
        leme_config_effective_copy(next, NULL, &plan->effective, error);
    if (copy_code != LEME_CONTROL_OK) {
      plan->next = NULL;
      leme_config_reload_discard(&plan);
      return copy_code;
    }
  }

  *out = plan;
  return LEME_CONTROL_OK;
}

void leme_config_reload_commit(struct leme_server *server,
                               struct leme_config_reload *plan) {
  if (server == NULL || plan == NULL) {
    return;
  }

  struct leme_config *old = server->config;
  struct leme_config *old_baseline = NULL;
  struct leme_config *next = plan->next;

  if (plan->resizes != NULL) {
    for (size_t i = 0; i < plan->resize_count; ++i) {
      leme_tags_commit_set_max(&plan->resizes[i]);
    }
    free(plan->resizes);
    plan->resizes = NULL;
    plan->resize_count = 0;
  }

  if (server->outputs.next != NULL) {
    struct leme_output *output;
    wl_list_for_each(output, &server->outputs, link) {
      leme_tags_apply_settings(leme_output_tags(output), old, next);
    }
  }

  leme_scratchpad_reconcile_config(server, old, next);

  if (server->config_store != NULL) {
    old_baseline = server->config_store->baseline;
    server->config_store->baseline = next;
    server->config_store->effective = plan->effective;
    server->config = plan->effective;
    leme_config_store_clear_overrides(server->config_store);
  } else {
    server->config = next;
  }
  plan->next = NULL;
  plan->effective = NULL;

  leme_public_server_config_changed(server);
  leme_input_public_keymap_committed(server);

  if (plan->cursor_manager != NULL) {
    leme_session_environment_cursor(server);
    leme_desktop_commit_cursor_config(server, plan->cursor_manager);
    plan->cursor_manager = NULL;
  }

  leme_input_replace_modes(server, next->modes, next->mode_count);
  leme_render_apply_fullscreen_coverage(server);
  leme_view_refresh_fullscreen(server);

  if (leme_output_focused(server) != NULL) {
    leme_tags_arrange_current(
        leme_focused_tags(server),
        leme_output_usable_box(leme_output_focused(server)), next->gap);
    leme_render_refresh_views(server);
    leme_tags_refresh_visibility(leme_focused_tags(server));
  }

  if (old_baseline != NULL && old_baseline != old) {
    leme_config_destroy(old_baseline);
  }
  leme_config_destroy(old);
}

void leme_config_reload_discard(struct leme_config_reload **plan_ptr) {
  if (plan_ptr == NULL || *plan_ptr == NULL) {
    return;
  }
  struct leme_config_reload *plan = *plan_ptr;
  *plan_ptr = NULL;

  if (plan->resizes != NULL) {
    for (size_t i = 0; i < plan->resize_count; ++i) {
      leme_tags_discard_set_max(&plan->resizes[i]);
    }
    free(plan->resizes);
  }
  if (plan->cursor_manager != NULL) {
    leme_desktop_discard_cursor_config(plan->cursor_manager);
  }
  if (plan->effective != NULL) {
    leme_config_destroy(plan->effective);
  }
  if (plan->next != NULL) {
    leme_config_destroy(plan->next);
  }
  if (plan->account != NULL) {
    leme_control_free(plan);
  } else {
    free(plan);
  }
}
