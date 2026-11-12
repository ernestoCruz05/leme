#ifndef LEME_CONFIG_LIVE_H
#define LEME_CONFIG_LIVE_H

#include "control/action.h"
#include "control/error.h"
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
                           struct leme_config **out,
                           struct leme_control_error *error);

void leme_config_live_finish(struct leme_server *server);

enum leme_control_code leme_config_store_add_override(
    struct leme_config_store *store, const struct leme_control_target *target,
    const char *const *path, size_t path_count,
    const struct leme_public_value *value, struct leme_control_error *error);

void leme_config_store_drop_target(struct leme_config_store *store,
                                   const struct leme_control_target *target);

void leme_config_store_clear_overrides(struct leme_config_store *store);

const struct leme_scoped_override *
leme_config_store_overrides(const struct leme_config_store *store,
                            size_t *out_count);

enum leme_control_code
leme_config_live_run(struct leme_server *server,
                     const struct leme_control_call *call, bool *applied,
                     struct leme_control_error *error);

#endif
