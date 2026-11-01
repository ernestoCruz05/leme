#include "control/control.h"

#include <stdlib.h>

struct leme_control_context {
  struct leme_server *server;
  struct leme_public_model *model;
  const struct leme_public_source *source;
  struct leme_control_limits limits;
  const struct leme_control_domain *domain;
  struct leme_control_scheduler *scheduler;
  leme_control_scheduler_destroy_fn scheduler_destroy;
};

enum leme_control_code
leme_control_context_create(struct leme_server *server,
                            struct leme_public_model *model,
                            const struct leme_public_source *source,
                            const struct leme_control_limits *limits,
                            const struct leme_control_domain *domain,
                            struct leme_control_context **out) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (limits != NULL && limits->subscriptions > 32)
    return LEME_CONTROL_INVALID_ARGUMENT;
  struct leme_control_context *ctx =
      malloc(sizeof(struct leme_control_context));
  if (ctx == NULL)
    return LEME_CONTROL_OUT_OF_MEMORY;
  ctx->server = server;
  ctx->model = model;
  ctx->source = source;
  ctx->limits = limits != NULL ? *limits : leme_control_limits_default();
  ctx->domain = domain;
  ctx->scheduler = NULL;
  ctx->scheduler_destroy = NULL;
  *out = ctx;
  return LEME_CONTROL_OK;
}

void leme_control_context_destroy(struct leme_control_context *context) {
  if (context == NULL)
    return;
  if (context->scheduler != NULL && context->scheduler_destroy != NULL)
    context->scheduler_destroy(context->scheduler);
  free(context);
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
