#ifndef LEME_CONFIG_LIVE_H
#define LEME_CONFIG_LIVE_H

#include "control/action.h"
#include "control/error.h"
#include "public/budget.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_server;
struct leme_config;
struct leme_config_store;

struct leme_scoped_override {
  bool has_target;
  struct leme_control_target target;
  char **path;
  size_t path_count;
  const struct leme_public_value *value;
};

enum leme_control_code
leme_config_effective_copy(const struct leme_config *baseline,
                           struct leme_public_budget *account,
                           struct leme_config **out,
                           struct leme_control_error *error);

bool leme_config_live_init(struct leme_server *server);
void leme_config_live_finish(struct leme_server *server);

enum leme_control_code
leme_config_store_add_override(struct leme_config_store *store,
                               const struct leme_control_target *target,
                               const char *const *path, size_t path_count,
                               const struct leme_public_value *value,
                               struct leme_control_error *error);

void leme_config_store_drop_target(struct leme_config_store *store,
                                   const struct leme_control_target *target);

void leme_config_store_clear_overrides(struct leme_config_store *store);

const struct leme_scoped_override *
leme_config_store_overrides(const struct leme_config_store *store,
                            size_t *out_count);

struct leme_config *
leme_config_store_baseline(const struct leme_config_store *store);

struct leme_config *
leme_config_store_effective(const struct leme_config_store *store);

struct leme_control_prepared;

enum leme_control_code
leme_config_live_prepare(struct leme_server *server,
                         const struct leme_control_intent *intents,
                         size_t count, struct leme_public_budget *account,
                         struct leme_control_prepared **out,
                         struct leme_control_error *error);

enum leme_control_code
leme_config_live_prepare_set(struct leme_server *server,
                             const char *const *path, size_t path_count,
                             const struct leme_public_value *value,
                             struct leme_public_budget *account,
                             struct leme_control_prepared **out,
                             struct leme_control_error *error);

enum leme_control_code
leme_config_live_execute_one(struct leme_server *server,
                             struct leme_control_prepared *prepared,
                             size_t index, enum leme_control_outcome *outcome,
                             struct leme_control_error *error);

void leme_config_live_discard(struct leme_server *server,
                              struct leme_control_prepared *prepared);

#endif
