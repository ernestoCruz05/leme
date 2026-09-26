#include "control/action-internal.h"
#include "control/control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

enum leme_control_code
leme_control_execute_action(struct leme_control_context *context,
                            struct leme_control_plan *plan,
                            struct leme_control_error *error) {
  if (context == NULL || plan == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  if (plan->executed) {
    return LEME_CONTROL_INVALID_REQUEST;
  }
  plan->executed = true;

  const struct leme_control_domain *domain = plan->domain;
  if (domain == NULL) {
    if (error != NULL) {
      error->code = LEME_CONTROL_UNSUPPORTED;
      error->phase = LEME_CONTROL_EXECUTE;
      (void)snprintf(error->message, sizeof(error->message), "unsupported");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_UNSUPPORTED;
  }

  const struct leme_public_source *source =
      leme_control_context_source(context);
  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  uint64_t deadline = leme_control_context_deadline(context);
  if (deadline == 0 && limits != NULL && limits->deadline_ns > 0) {
    deadline = monotonic_now_ns(NULL) + limits->deadline_ns;
  }

  if (plan->count == 0) {
    return LEME_CONTROL_OK;
  }

  for (size_t i = 0; i < plan->count; ++i) {
    if (deadline > 0 && monotonic_now_ns(NULL) >= deadline) {
      plan->outcomes[i] = LEME_CONTROL_FAILED;
      for (size_t j = i + 1; j < plan->count; ++j) {
        plan->outcomes[j] = LEME_CONTROL_UNATTEMPTED;
      }
      if (error != NULL) {
        error->code = LEME_CONTROL_RESOURCE_LIMIT;
        error->phase = LEME_CONTROL_EXECUTE;
        (void)snprintf(error->message, sizeof(error->message),
                       "deadline exceeded");
        (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
        error->effects_applied = plan->effects_applied;
      }
      return LEME_CONTROL_RESOURCE_LIMIT;
    }

    if (source != NULL && source->locked != NULL &&
        source->locked(source->context)) {
      plan->outcomes[i] = LEME_CONTROL_FAILED;
      for (size_t j = i + 1; j < plan->count; ++j) {
        plan->outcomes[j] = LEME_CONTROL_UNATTEMPTED;
      }
      if (error != NULL) {
        error->code = LEME_CONTROL_SESSION_LOCKED;
        error->phase = LEME_CONTROL_EXECUTE;
        (void)snprintf(error->message, sizeof(error->message),
                       "session is locked");
        (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
        error->effects_applied = plan->effects_applied;
      }
      return LEME_CONTROL_SESSION_LOCKED;
    }

    enum leme_control_outcome outcome = LEME_CONTROL_UNATTEMPTED;
    enum leme_control_code code = LEME_CONTROL_OK;
    if (domain->execute_one != NULL) {
      code = domain->execute_one(domain->context, plan->prepared, i, &outcome,
                                 error);
    } else {
      code = LEME_CONTROL_UNSUPPORTED;
    }

    if (code != LEME_CONTROL_OK) {
      plan->outcomes[i] = LEME_CONTROL_FAILED;
      if (error != NULL) {
        if (error->effects_applied) {
          plan->effects_applied = true;
        } else {
          error->effects_applied = plan->effects_applied;
        }
        if (error->phase == 0) {
          error->phase = LEME_CONTROL_EXECUTE;
        }
        if (error->expr_path[0] == '\0') {
          (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
        }
      }
      for (size_t j = i + 1; j < plan->count; ++j) {
        plan->outcomes[j] = LEME_CONTROL_UNATTEMPTED;
      }
      return code;
    }

    plan->outcomes[i] = outcome;
    if (outcome == LEME_CONTROL_APPLIED || outcome == LEME_CONTROL_ACCEPTED) {
      plan->affected++;
      plan->effects_applied = true;
    }
  }

  return LEME_CONTROL_OK;
}
