#ifndef LEME_CONFIG_LIVE_INTERNAL_H
#define LEME_CONFIG_LIVE_INTERNAL_H

#include "config/live.h"
#include "control/action.h"
#include "public/budget.h"
#include "public/value.h"


struct leme_config_store {
  struct leme_server *server;
  struct leme_config *baseline;
  struct leme_config *effective;
  struct leme_scoped_override *overrides;
  size_t override_count;
  size_t override_capacity;
  struct leme_public_budget *account;
  struct leme_public_builder *builder;
};

#endif
