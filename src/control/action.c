#include "control/action-internal.h"
#include "control/limits.h"
#include "control/memory.h"
#include "public/model.h"
#include "public/value.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

enum leme_control_code leme_control_prepare_action(
    struct leme_control_context *context,
    const struct leme_control_program *program,
    const struct leme_public_snapshot *snapshot,
    struct leme_control_plan **out, struct leme_control_error *error) {
  if (context == NULL || program == NULL || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_domain *domain =
      leme_control_context_domain(context);
  if (domain == NULL || domain->prepare == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_UNSUPPORTED;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message),
                     "actions not supported");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_UNSUPPORTED;
  }

  struct leme_control_intent *intents = NULL;
  size_t intent_count = 0;
  struct leme_public_builder *args_builder = NULL;
  enum leme_control_code code = leme_control_normalize_targets(
      context, program, snapshot, &intents, &intent_count, &args_builder,
      error);
  if (code != LEME_CONTROL_OK) {
    return code;
  }

  struct leme_control_plan *plan =
      leme_control_alloc(account, sizeof(struct leme_control_plan));
  if (plan == NULL) {
    leme_control_intents_destroy(intents, intent_count);
    if (args_builder != NULL) {
      leme_public_builder_destroy(args_builder);
    }
    if (error != NULL) {
      error->code = LEME_CONTROL_OUT_OF_MEMORY;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message),
                     "out of memory");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  memset(plan, 0, sizeof(*plan));
  plan->context = context;
  plan->account = account;
  plan->domain = domain;
  plan->intents = intents;
  plan->count = intent_count;
  plan->args_builder = args_builder;

  if (intent_count > 0) {
    plan->outcomes = leme_control_alloc(
        account, intent_count * sizeof(enum leme_control_outcome));
    if (plan->outcomes == NULL) {
      leme_control_intents_destroy(intents, intent_count);
      leme_control_free(plan);
      if (error != NULL) {
        error->code = LEME_CONTROL_OUT_OF_MEMORY;
        error->phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(error->message, sizeof(error->message),
                       "out of memory");
        (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
        error->effects_applied = false;
      }
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < intent_count; ++i) {
      plan->outcomes[i] = LEME_CONTROL_UNATTEMPTED;
    }
  }

  if (snapshot != NULL) {
    struct leme_public_text rev = leme_public_snapshot_revision(snapshot);
    size_t copy_len = rev.length < sizeof(plan->revision_buf) - 1
                          ? rev.length
                          : sizeof(plan->revision_buf) - 1;
    if (copy_len > 0 && rev.data != NULL) {
      memcpy(plan->revision_buf, rev.data, copy_len);
    }
    plan->revision_buf[copy_len] = '\0';
    plan->selection_revision = (struct leme_public_text){
        .data = plan->revision_buf,
        .length = copy_len,
    };
  }

  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  uint64_t deadline = leme_control_context_deadline(context);
  if (deadline == 0 && limits != NULL && limits->deadline_ns > 0) {
    deadline = monotonic_now_ns(NULL) + limits->deadline_ns;
  }
  struct leme_control_meter meter = {
      .remaining = limits != NULL ? limits->work_units : 100000,
      .deadline_ns = deadline,
      .context = NULL,
      .now_ns = monotonic_now_ns,
  };
  enum leme_control_code charge_code =
      leme_control_charge(&meter, intent_count > 0 ? intent_count : 1);
  if (charge_code != LEME_CONTROL_OK) {
    leme_control_plan_destroy(plan);
    if (error != NULL) {
      error->code = charge_code;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message),
                     "domain preparation work limit exceeded");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return charge_code;
  }

  struct leme_control_prepared *prepared = NULL;
  code = domain->prepare(domain->context, intents, intent_count, account,
                         &prepared, error);
  if (code != LEME_CONTROL_OK) {
    if (error != NULL && error->phase == 0) {
      error->phase = LEME_CONTROL_PREFLIGHT;
      error->effects_applied = false;
    }
    leme_control_plan_destroy(plan);
    return code;
  }

  if (meter.now_ns != NULL && meter.deadline_ns > 0 &&
      meter.now_ns(meter.context) >= meter.deadline_ns) {
    if (domain->discard != NULL && prepared != NULL) {
      domain->discard(domain->context, prepared);
      prepared = NULL;
    }
    leme_control_plan_destroy(plan);
    if (error != NULL) {
      error->code = LEME_CONTROL_RESOURCE_LIMIT;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message),
                     "domain preparation deadline exceeded");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_RESOURCE_LIMIT;
  }

  plan->prepared = prepared;
  *out = plan;
  return LEME_CONTROL_OK;
}

void leme_control_plan_destroy(struct leme_control_plan *plan) {
  if (plan == NULL) {
    return;
  }
  if (plan->domain != NULL && plan->domain->discard != NULL &&
      plan->prepared != NULL) {
    plan->domain->discard(plan->domain->context, plan->prepared);
    plan->prepared = NULL;
  }
  if (plan->outcomes != NULL) {
    leme_control_free(plan->outcomes);
  }
  if (plan->intents != NULL) {
    leme_control_intents_destroy(plan->intents, plan->count);
  }
  if (plan->args_builder != NULL) {
    leme_public_builder_destroy(plan->args_builder);
  }
  leme_control_free(plan);
}

struct leme_public_text
leme_control_plan_revision(const struct leme_control_plan *plan) {
  if (plan == NULL) {
    return (struct leme_public_text){0};
  }
  return plan->selection_revision;
}

size_t leme_control_plan_count(const struct leme_control_plan *plan) {
  if (plan == NULL) {
    return 0;
  }
  return plan->count;
}

enum leme_control_outcome
leme_control_plan_outcome(const struct leme_control_plan *plan, size_t index) {
  if (plan == NULL || plan->outcomes == NULL || index >= plan->count) {
    return LEME_CONTROL_UNATTEMPTED;
  }
  return plan->outcomes[index];
}

size_t leme_control_plan_affected(const struct leme_control_plan *plan) {
  if (plan == NULL) {
    return 0;
  }
  return plan->affected;
}

bool leme_control_plan_effects_applied(const struct leme_control_plan *plan) {
  if (plan == NULL) {
    return false;
  }
  return plan->effects_applied;
}

struct leme_public_text
leme_control_plan_requested_id(const struct leme_control_plan *plan,
                               size_t index) {
  if (plan == NULL || plan->intents == NULL || index >= plan->count) {
    return (struct leme_public_text){0};
  }
  return plan->intents[index].requested_id;
}

struct leme_public_text
leme_control_plan_effective_id(const struct leme_control_plan *plan,
                               size_t index) {
  if (plan == NULL || plan->intents == NULL || index >= plan->count) {
    return (struct leme_public_text){0};
  }
  return plan->intents[index].effective_id;
}

const struct leme_control_target *
leme_control_plan_target(const struct leme_control_plan *plan, size_t index) {
  if (plan == NULL || plan->intents == NULL || index >= plan->count) {
    return NULL;
  }
  return plan->intents[index].has_target ? &plan->intents[index].target : NULL;
}

size_t leme_control_plan_requested_count(const struct leme_control_plan *plan,
                                         size_t index) {
  if (plan == NULL || plan->intents == NULL || index >= plan->count) {
    return 0;
  }
  return plan->intents[index].requested_count;
}

struct leme_public_text
leme_control_plan_requested_target(const struct leme_control_plan *plan,
                                   size_t index, size_t req_index) {
  if (plan == NULL || plan->intents == NULL || index >= plan->count) {
    return (struct leme_public_text){0};
  }
  if (req_index >= plan->intents[index].requested_count ||
      plan->intents[index].requested_targets == NULL) {
    return (struct leme_public_text){0};
  }
  return plan->intents[index].requested_targets[req_index];
}

static const char *outcome_to_str(enum leme_control_outcome outcome) {
  switch (outcome) {
  case LEME_CONTROL_APPLIED:
    return "applied";
  case LEME_CONTROL_ACCEPTED:
    return "accepted";
  case LEME_CONTROL_NOOP:
    return "noop";
  case LEME_CONTROL_FAILED:
    return "failed";
  case LEME_CONTROL_UNATTEMPTED:
    return "unattempted";
  }
  return "unattempted";
}

static enum leme_public_status
build_results_array(const struct leme_control_plan *plan,
                    struct leme_public_builder *builder,
                    struct leme_public_value **out) {
  struct leme_public_value *results_arr = NULL;
  enum leme_public_status st =
      leme_public_array(builder, plan->count, &results_arr);
  if (st != LEME_PUBLIC_OK) {
    return st;
  }

  for (size_t i = 0; i < plan->count; ++i) {
    const struct leme_control_intent *intent = &plan->intents[i];
    struct leme_public_value *row = NULL;
    st = leme_public_object(builder, intent->has_target ? 3 : 4, &row);
    if (st != LEME_PUBLIC_OK) {
      return st;
    }

    if (intent->has_target) {
      if (intent->effective_id.data != NULL) {
        st = leme_public_put_cstr(builder, row, LEME_PUBLIC_TEXT("target"),
                                  intent->effective_id.data);
      } else {
        st = leme_public_put_null(builder, row, LEME_PUBLIC_TEXT("target"));
      }
      if (st != LEME_PUBLIC_OK) {
        return st;
      }
    } else {
      st = leme_public_put_null(builder, row, LEME_PUBLIC_TEXT("target"));
      if (st != LEME_PUBLIC_OK) {
        return st;
      }
      const struct leme_control_operator *op_info =
          leme_control_operator_by_opcode(intent->opcode);
      struct leme_public_text op_name =
          op_info != NULL ? op_info->name : LEME_PUBLIC_TEXT("unknown");
      st = leme_public_put_text(builder, row, LEME_PUBLIC_TEXT("operation"),
                                op_name, false);
      if (st != LEME_PUBLIC_OK) {
        return st;
      }
    }

    struct leme_public_value *req_arr = NULL;
    st = leme_public_array(builder, intent->requested_count, &req_arr);
    if (st != LEME_PUBLIC_OK) {
      return st;
    }
    for (size_t r = 0; r < intent->requested_count; ++r) {
      struct leme_public_value *r_val = NULL;
      if (intent->requested_targets != NULL &&
          intent->requested_targets[r].data != NULL) {
        st = leme_public_string(builder, intent->requested_targets[r], false,
                                &r_val);
      } else {
        st = leme_public_null(builder, &r_val);
      }
      if (st != LEME_PUBLIC_OK) {
        return st;
      }
      st = leme_public_array_set(builder, req_arr, r, r_val);
      if (st != LEME_PUBLIC_OK) {
        return st;
      }
    }
    st = leme_public_object_set(builder, row,
                                LEME_PUBLIC_TEXT("requested_targets"),
                                req_arr);
    if (st != LEME_PUBLIC_OK) {
      return st;
    }

    enum leme_control_outcome outcome =
        plan->outcomes != NULL ? plan->outcomes[i] : LEME_CONTROL_UNATTEMPTED;
    const char *status_str = outcome_to_str(outcome);

    st = leme_public_put_cstr(builder, row, LEME_PUBLIC_TEXT("status"),
                              status_str);
    if (st != LEME_PUBLIC_OK) {
      return st;
    }

    st = leme_public_array_set(builder, results_arr, i, row);
    if (st != LEME_PUBLIC_OK) {
      return st;
    }
  }

  *out = results_arr;
  return LEME_PUBLIC_OK;
}

enum leme_control_code
leme_control_plan_details(const struct leme_control_plan *plan,
                          struct leme_public_builder *builder,
                          struct leme_public_value **out) {
  if (plan == NULL || builder == NULL || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  struct leme_public_value *results_arr = NULL;
  enum leme_public_status st = build_results_array(plan, builder, &results_arr);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  struct leme_public_value *val = NULL;
  st = leme_public_object(builder, 3, &val);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  st = leme_public_put_bool(builder, val, LEME_PUBLIC_TEXT("effects_applied"),
                            leme_control_plan_effects_applied(plan));
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  if (plan->selection_revision.data != NULL &&
      plan->selection_revision.length > 0) {
    st = leme_public_put_text(builder, val,
                              LEME_PUBLIC_TEXT("selection_revision"),
                              plan->selection_revision, false);
  } else {
    st = leme_public_put_null(builder, val,
                              LEME_PUBLIC_TEXT("selection_revision"));
  }
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  st = leme_public_object_set(builder, val, LEME_PUBLIC_TEXT("results"),
                              results_arr);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  *out = val;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_control_plan_value(const struct leme_control_plan *plan,
                        struct leme_public_builder *builder,
                        const char *warning_code,
                        struct leme_public_value **out) {
  if (plan == NULL || builder == NULL || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  struct leme_public_value *results_arr = NULL;
  enum leme_public_status st = build_results_array(plan, builder, &results_arr);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  struct leme_public_value *val = NULL;
  st = leme_public_object(builder, 4, &val);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  if (plan->selection_revision.data != NULL &&
      plan->selection_revision.length > 0) {
    st = leme_public_put_text(builder, val,
                              LEME_PUBLIC_TEXT("selection_revision"),
                              plan->selection_revision, false);
  } else {
    st = leme_public_put_null(builder, val,
                              LEME_PUBLIC_TEXT("selection_revision"));
  }
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  st = leme_public_object_set(builder, val, LEME_PUBLIC_TEXT("results"),
                              results_arr);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  st = leme_public_put_int(builder, val, LEME_PUBLIC_TEXT("affected"),
                           (int64_t)plan->affected);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  struct leme_public_value *warn_arr = NULL;
  if (warning_code != NULL) {
    st = leme_public_array(builder, 1, &warn_arr);
    if (st != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    struct leme_public_value *warn_item = NULL;
    st = leme_public_object(builder, 1, &warn_item);
    if (st != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    st = leme_public_put_cstr(builder, warn_item, LEME_PUBLIC_TEXT("code"),
                              warning_code);
    if (st != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    st = leme_public_array_set(builder, warn_arr, 0, warn_item);
    if (st != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  } else {
    st = leme_public_array(builder, 0, &warn_arr);
    if (st != LEME_PUBLIC_OK) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }

  st = leme_public_object_set(builder, val, LEME_PUBLIC_TEXT("warnings"),
                              warn_arr);
  if (st != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  *out = val;
  return LEME_CONTROL_OK;
}
