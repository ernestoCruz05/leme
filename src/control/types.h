#ifndef LEME_CONTROL_TYPES_H
#define LEME_CONTROL_TYPES_H

#include "public/identity.h"
#include "public/schema.h"
#include "public/value.h"

#include <stdbool.h>

enum leme_control_type_kind {
  LEME_CONTROL_TYPE_UNKNOWN = 0,
  LEME_CONTROL_TYPE_ANY,
  LEME_CONTROL_TYPE_NULL,
  LEME_CONTROL_TYPE_BOOLEAN,
  LEME_CONTROL_TYPE_NUMBER,
  LEME_CONTROL_TYPE_STRING,
  LEME_CONTROL_TYPE_ARRAY,
  LEME_CONTROL_TYPE_RECORD,
  LEME_CONTROL_TYPE_ENTITY,
  LEME_CONTROL_TYPE_FIELD
};

enum leme_control_provenance {
  LEME_CONTROL_PROVENANCE_NONE = 0,
  LEME_CONTROL_PROVENANCE_SYNTHETIC,
  LEME_CONTROL_PROVENANCE_SNAPSHOT
};

struct leme_control_type {
  enum leme_control_type_kind kind;
  bool nullable;
  enum leme_control_provenance provenance;
  enum leme_public_entity entity;
  const struct leme_public_schema *schema;
  const struct leme_control_type *item_type;
  const struct leme_control_type *alternative;
};

struct leme_control_type leme_control_type_any(void);
struct leme_control_type leme_control_type_null(void);
struct leme_control_type leme_control_type_boolean(void);
struct leme_control_type leme_control_type_number(void);
struct leme_control_type leme_control_type_string(void);
struct leme_control_type
leme_control_type_entity(enum leme_public_entity entity,
                         enum leme_control_provenance provenance);
struct leme_control_type
leme_control_type_record(const struct leme_public_schema *schema);
struct leme_control_type
leme_control_type_array(const struct leme_control_type *item);

bool leme_control_type_equal(const struct leme_control_type *left,
                             const struct leme_control_type *right);
bool leme_control_type_compatible(const struct leme_control_type *expected,
                                  const struct leme_control_type *actual);
bool leme_control_type_is_trusted_entity(const struct leme_control_type *type);

const struct leme_control_type *
leme_control_type_entity_ref(enum leme_public_entity entity,
                             enum leme_control_provenance provenance);
const struct leme_control_type *
leme_control_type_collection_ref(enum leme_public_entity entity,
                                 enum leme_control_provenance provenance);
struct leme_control_type
leme_control_type_from_value(const struct leme_public_value *value);

#endif
