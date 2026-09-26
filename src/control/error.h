#ifndef LEME_CONTROL_ERROR_H
#define LEME_CONTROL_ERROR_H

#include <stdbool.h>
#include <stddef.h>

enum leme_control_code {
  LEME_CONTROL_OK,
  LEME_CONTROL_INVALID_JSON,
  LEME_CONTROL_INVALID_REQUEST,
  LEME_CONTROL_UNSUPPORTED_VERSION,
  LEME_CONTROL_STALE_INSTANCE,
  LEME_CONTROL_UNKNOWN_OPERATOR,
  LEME_CONTROL_UNKNOWN_FIELD,
  LEME_CONTROL_TYPE_ERROR,
  LEME_CONTROL_INVALID_ARGUMENT,
  LEME_CONTROL_NOT_FOUND,
  LEME_CONTROL_CARDINALITY,
  LEME_CONTROL_CONFLICTING_TARGETS,
  LEME_CONTROL_UNSUPPORTED,
  LEME_CONTROL_SESSION_LOCKED,
  LEME_CONTROL_RESOURCE_LIMIT,
  LEME_CONTROL_OUT_OF_MEMORY,
  LEME_CONTROL_ACTION_FAILED
};

enum leme_control_phase {
  LEME_CONTROL_DECODE,
  LEME_CONTROL_VALIDATE,
  LEME_CONTROL_EVALUATE,
  LEME_CONTROL_PREFLIGHT,
  LEME_CONTROL_EXECUTE
};

struct leme_public_value;

struct leme_control_error {
  enum leme_control_code code;
  enum leme_control_phase phase;
  char message[192];
  char expr_path[1024];
  size_t byte_offset;
  bool has_byte_offset;
  bool effects_applied;
  bool sensitive;
  const struct leme_public_value *details;
};

const char *leme_control_code_to_string(enum leme_control_code code);

#endif
