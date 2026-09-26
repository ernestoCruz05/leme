#ifndef LEME_PUBLIC_SCHEMA_INTERNAL_H
#define LEME_PUBLIC_SCHEMA_INTERNAL_H

#include "public/schema.h"

enum leme_public_schema_id {
  LEME_SCHEMA_NONE,
#define PUBLIC_SCALAR(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_REFERENCE(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_SCHEMA(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_ARRAY(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_NUMBER(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_ENUM(id, ...) LEME_SCHEMA_##id,
#define PUBLIC_UNION(id, ...) LEME_SCHEMA_##id,
#include "public/schema.def"
#undef PUBLIC_SCALAR
#undef PUBLIC_REFERENCE
#undef PUBLIC_SCHEMA
#undef PUBLIC_ARRAY
#undef PUBLIC_NUMBER
#undef PUBLIC_ENUM
#undef PUBLIC_UNION
  LEME_SCHEMA_TYPE_COUNT
};
enum leme_public_access { LEME_ACCESS_SAFE, LEME_ACCESS_SENSITIVE };
enum leme_public_condition {
  LEME_CONDITION_ALWAYS,
  LEME_CONDITION_EFFECTS,
  LEME_CONDITION_BANNER
};
enum leme_public_rule {
  LEME_RULE_NONE,
  LEME_RULE_NONEMPTY,
  LEME_RULE_COLOR,
  LEME_RULE_DECIMAL,
  LEME_RULE_TRUE,
  LEME_RULE_FINGERS,
  LEME_RULE_ANY
};
struct leme_public_field {
  struct leme_public_text name;
  const struct leme_public_schema *type;
  bool nullable;
  bool required;
  const char *units;
  enum leme_public_access access;
  enum leme_public_condition condition;
  const char *write_capability;
  const char *description;
};
struct leme_public_schema {
  const char *name;
  enum leme_public_kind kind;
  enum leme_public_rule rule;
  bool sparse;
  bool integer;
  bool has_minimum;
  bool has_maximum;
  double minimum;
  double maximum;
  const char *const *enum_values;
  size_t enum_count;
  const struct leme_public_schema *related;
  const struct leme_public_schema *items;
  const struct leme_public_schema *primary;
  const struct leme_public_schema *alternative;
  const struct leme_public_field *fields;
  size_t field_count;
};
struct leme_public_root_descriptor {
  const char *name;
  const struct leme_public_schema *type;
  enum leme_public_access access;
};
struct leme_public_registry {
  const struct leme_public_schema *types;
  size_t type_count;
  const struct leme_public_root_descriptor *roots;
  size_t root_count;
};

enum leme_public_status
leme_public_registry_validate(const struct leme_public_registry *registry);

#endif
