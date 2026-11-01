#ifndef LEME_CONTROL_ACTION_H
#define LEME_CONTROL_ACTION_H

#include "control/control.h"
#include "control/error.h"
#include "control/expr.h"
#include "control/registry.h"
#include "public/identity.h"
#include "public/model.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_control_target {
  enum leme_public_entity kind;
  struct leme_public_id id;
  uint16_t tag_number;
};

struct leme_control_call {
  enum leme_control_opcode opcode;
  const struct leme_control_target *target;
  const struct leme_control_target *destination;
  const struct leme_public_value *const *args;
  size_t count;
};

struct leme_control_domain {
  void *context;
  enum leme_control_code (*run)(void *context,
                                const struct leme_control_call *call,
                                bool *applied,
                                struct leme_control_error *error);
};

enum leme_control_code
leme_control_act(struct leme_control_context *context,
                 const struct leme_control_program *program,
                 const struct leme_public_snapshot *snapshot, size_t *affected,
                 struct leme_control_error *error);

enum leme_control_code leme_control_refuse(struct leme_control_error *error,
                                           enum leme_control_code code,
                                           const char *message);

enum leme_control_code leme_control_fail(struct leme_control_error *error,
                                         enum leme_control_code code,
                                         const char *message);

#endif
