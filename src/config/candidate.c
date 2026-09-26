#include "config/live.h"
#include "config/config.h"
#include "control/error.h"
#include "public/budget.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct leme_config *unconst_config(const struct leme_config *p) {
  void *out = NULL;
  memcpy((void *)&out, (const void *)&p, sizeof(out));
  return (struct leme_config *)out;
}

enum leme_control_code
leme_config_effective_copy(const struct leme_config *baseline,
                           struct leme_public_budget *account,
                           struct leme_config **out,
                           struct leme_control_error *error) {
  if (out == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_INVALID_ARGUMENT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid null output pointer");
    }
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (baseline == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_INVALID_ARGUMENT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "invalid null baseline pointer");
    }
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  if (account != NULL) {
    if (leme_public_budget_reserve(account, sizeof(struct leme_config)) !=
        LEME_PUBLIC_OK) {
      if (error != NULL) {
        error->code = LEME_CONTROL_RESOURCE_LIMIT;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       "budget exceeded for effective config copy");
      }
      return LEME_CONTROL_RESOURCE_LIMIT;
    }
  }
  struct leme_config *effective = calloc(1, sizeof(*effective));
  if (effective == NULL) {
    if (account != NULL) {
      leme_public_budget_release(account, sizeof(struct leme_config));
    }
    if (error != NULL) {
      error->code = LEME_CONTROL_OUT_OF_MEMORY;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message), "%s",
                     "out of memory allocating effective config");
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  *effective = *baseline;
  effective->refcount = 1;
  effective->is_effective_shell = true;
  effective->baseline = leme_config_ref(unconst_config(baseline));
  effective->cursor.theme = NULL;
  effective->path = NULL;
  if (baseline->cursor.theme != NULL) {
    effective->cursor.theme = strdup(baseline->cursor.theme);
    if (effective->cursor.theme == NULL) {
      leme_config_destroy(effective);
      if (account != NULL) {
        leme_public_budget_release(account, sizeof(struct leme_config));
      }
      if (error != NULL) {
        error->code = LEME_CONTROL_OUT_OF_MEMORY;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       "out of memory duplicating cursor theme");
      }
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }
  if (baseline->path != NULL) {
    effective->path = strdup(baseline->path);
    if (effective->path == NULL) {
      leme_config_destroy(effective);
      if (account != NULL) {
        leme_public_budget_release(account, sizeof(struct leme_config));
      }
      if (error != NULL) {
        error->code = LEME_CONTROL_OUT_OF_MEMORY;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       "out of memory duplicating config path");
      }
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }
  *out = effective;
  return LEME_CONTROL_OK;
}
