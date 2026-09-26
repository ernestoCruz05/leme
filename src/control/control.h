#ifndef LEME_CONTROL_CONTROL_H
#define LEME_CONTROL_CONTROL_H

#include "control/error.h"
#include "control/limits.h"
#include "public/budget.h"
#include "public/model.h"

struct leme_server;
struct leme_control_domain;

struct leme_control_context;

enum leme_control_code leme_control_context_create(
    struct leme_server *server, struct leme_public_model *model,
    const struct leme_public_source *source, struct leme_public_budget *account,
    const struct leme_control_limits *limits,
    const struct leme_control_domain *domain,
    struct leme_control_context **out);

void leme_control_context_destroy(struct leme_control_context *context);

struct leme_public_budget *
leme_control_context_account(const struct leme_control_context *context);

const struct leme_control_limits *
leme_control_context_limits(const struct leme_control_context *context);

struct leme_server *
leme_control_context_server(const struct leme_control_context *context);

struct leme_public_model *
leme_control_context_model(const struct leme_control_context *context);

const struct leme_public_source *
leme_control_context_source(const struct leme_control_context *context);

const struct leme_control_domain *
leme_control_context_domain(const struct leme_control_context *context);

struct leme_control_scheduler;

struct leme_control_scheduler *
leme_control_context_scheduler(const struct leme_control_context *context);

typedef void (*leme_control_scheduler_destroy_fn)(
    struct leme_control_scheduler *);

void leme_control_context_set_scheduler(
    struct leme_control_context *context,
    struct leme_control_scheduler *scheduler,
    leme_control_scheduler_destroy_fn destroy_fn);

void leme_control_context_set_deadline(struct leme_control_context *context,
                                       uint64_t deadline_ns);

uint64_t
leme_control_context_deadline(const struct leme_control_context *context);

#endif
