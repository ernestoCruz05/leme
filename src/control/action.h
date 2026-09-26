#ifndef LEME_CONTROL_ACTION_H
#define LEME_CONTROL_ACTION_H

#include "control/control.h"
#include "control/error.h"
#include "control/expr.h"
#include "control/limits.h"
#include "control/registry.h"
#include "public/budget.h"
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

enum leme_control_outcome {
  LEME_CONTROL_APPLIED,
  LEME_CONTROL_ACCEPTED,
  LEME_CONTROL_NOOP,
  LEME_CONTROL_FAILED,
  LEME_CONTROL_UNATTEMPTED
};

struct leme_control_intent {
  enum leme_control_opcode opcode;
  bool has_target;
  struct leme_control_target target;
  struct leme_public_text requested_id;
  struct leme_public_text effective_id;
  size_t requested_count;
  struct leme_public_text *requested_targets;
  bool has_destination;
  struct leme_control_target destination;
  const struct leme_public_value *args;
};

struct leme_control_prepared;

struct leme_control_domain {
  void *context;
  enum leme_control_code (*prepare)(
      void *context, const struct leme_control_intent *intents, size_t count,
      struct leme_public_budget *account,
      struct leme_control_prepared **out, struct leme_control_error *error);
  enum leme_control_code (*execute_one)(
      void *context, struct leme_control_prepared *prepared, size_t index,
      enum leme_control_outcome *outcome, struct leme_control_error *error);
  void (*discard)(void *context, struct leme_control_prepared *prepared);
};

struct leme_control_plan;

enum leme_control_code leme_control_prepare_action(
    struct leme_control_context *context,
    const struct leme_control_program *program,
    const struct leme_public_snapshot *snapshot,
    struct leme_control_plan **out, struct leme_control_error *error);

enum leme_control_code leme_control_execute_action(
    struct leme_control_context *context,
    struct leme_control_plan *plan,
    struct leme_control_error *error);

void leme_control_plan_destroy(struct leme_control_plan *plan);

struct leme_public_text leme_control_plan_revision(const struct leme_control_plan *plan);
size_t leme_control_plan_count(const struct leme_control_plan *plan);
enum leme_control_outcome leme_control_plan_outcome(const struct leme_control_plan *plan, size_t index);
size_t leme_control_plan_affected(const struct leme_control_plan *plan);
bool leme_control_plan_effects_applied(const struct leme_control_plan *plan);
struct leme_public_text leme_control_plan_requested_id(const struct leme_control_plan *plan, size_t index);
struct leme_public_text leme_control_plan_effective_id(const struct leme_control_plan *plan, size_t index);
const struct leme_control_target *leme_control_plan_target(const struct leme_control_plan *plan, size_t index);
size_t leme_control_plan_requested_count(const struct leme_control_plan *plan, size_t index);
struct leme_public_text leme_control_plan_requested_target(const struct leme_control_plan *plan, size_t index, size_t req_index);

enum leme_control_code leme_control_plan_value(
    const struct leme_control_plan *plan,
    struct leme_public_builder *builder,
    const char *warning_code,
    struct leme_public_value **out);

enum leme_control_code leme_control_plan_details(
    const struct leme_control_plan *plan,
    struct leme_public_builder *builder,
    struct leme_public_value **out);

#endif
