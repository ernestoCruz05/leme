#include "control/control.h"
#include "control/memory.h"

#include <stdlib.h>

struct leme_control_context {
  struct leme_server *server;
  struct leme_public_model *model;
  const struct leme_public_source *source;
  struct leme_public_budget *account;
  struct leme_control_limits limits;
  const struct leme_control_domain *domain;
  struct leme_control_scheduler *scheduler;
  leme_control_scheduler_destroy_fn scheduler_destroy;
  uint64_t deadline_ns;
};

enum leme_control_code leme_control_context_create(
    struct leme_server *server, struct leme_public_model *model,
    const struct leme_public_source *source,
    struct leme_public_budget *account,
    const struct leme_control_limits *limits,
    const struct leme_control_domain *domain,
    struct leme_control_context **out) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (account == NULL || (limits != NULL && limits->subscriptions > 32))
    return LEME_CONTROL_INVALID_ARGUMENT;
  struct leme_control_context *ctx =
      leme_control_alloc(account, sizeof(struct leme_control_context));
  if (ctx == NULL)
    return LEME_CONTROL_OUT_OF_MEMORY;
  ctx->server = server;
  ctx->model = model;
  ctx->source = source;
  ctx->account = account;
  ctx->limits = limits != NULL ? *limits : leme_control_limits_default();
  ctx->domain = domain;
  ctx->scheduler = NULL;
  ctx->scheduler_destroy = NULL;
  ctx->deadline_ns = 0;
  *out = ctx;
  return LEME_CONTROL_OK;
}

void leme_control_context_destroy(struct leme_control_context *context) {
  if (context == NULL)
    return;
  if (context->scheduler != NULL && context->scheduler_destroy != NULL)
    context->scheduler_destroy(context->scheduler);
  leme_control_free(context);
}

struct leme_public_budget *
leme_control_context_account(const struct leme_control_context *context) {
  return context != NULL ? context->account : NULL;
}

const struct leme_control_limits *
leme_control_context_limits(const struct leme_control_context *context) {
  return context != NULL ? &context->limits : NULL;
}

struct leme_server *
leme_control_context_server(const struct leme_control_context *context) {
  return context != NULL ? context->server : NULL;
}

struct leme_public_model *
leme_control_context_model(const struct leme_control_context *context) {
  return context != NULL ? context->model : NULL;
}

const struct leme_public_source *
leme_control_context_source(const struct leme_control_context *context) {
  return context != NULL ? context->source : NULL;
}

const struct leme_control_domain *
leme_control_context_domain(const struct leme_control_context *context) {
  return context != NULL ? context->domain : NULL;
}

struct leme_control_scheduler *
leme_control_context_scheduler(const struct leme_control_context *context) {
  return context != NULL ? context->scheduler : NULL;
}

void leme_control_context_set_scheduler(
    struct leme_control_context *context,
    struct leme_control_scheduler *scheduler,
    leme_control_scheduler_destroy_fn destroy_fn) {
  if (context != NULL) {
    context->scheduler = scheduler;
    context->scheduler_destroy = destroy_fn;
  }
}

void leme_control_context_set_deadline(struct leme_control_context *context,
                                       uint64_t deadline_ns) {
  if (context != NULL) {
    context->deadline_ns = deadline_ns;
  }
}

uint64_t
leme_control_context_deadline(const struct leme_control_context *context) {
  return context != NULL ? context->deadline_ns : 0;
}
