#ifndef LEME_PUBLIC_SCHEMA_H
#define LEME_PUBLIC_SCHEMA_H

#include "public/identity.h"
#include "public/value.h"

#define LEME_PUBLIC_MAX_FIELD_DEPTH 16u

enum leme_public_root {
  LEME_PUBLIC_VIEWS,
  LEME_PUBLIC_TAGS,
  LEME_PUBLIC_OUTPUTS,
  LEME_PUBLIC_INPUTS,
  LEME_PUBLIC_SESSION,
  LEME_PUBLIC_CONFIG,
  LEME_PUBLIC_RUNTIME,
  LEME_PUBLIC_STATUS,
  LEME_PUBLIC_ROOT_COUNT
};
#define LEME_PUBLIC_ROOT_BIT(r) (UINT32_C(1) << (unsigned)(r))
#define LEME_PUBLIC_SAFE_ROOTS                                                 \
  (LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_RUNTIME) |                                 \
   LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_STATUS))
#define LEME_PUBLIC_ALL_ROOTS                                                  \
  ((UINT32_C(1) << (unsigned)LEME_PUBLIC_ROOT_COUNT) - UINT32_C(1))

struct leme_public_schema;
struct leme_public_field;
struct leme_public_features {
  bool effects_build;
  bool effects_runtime_known;
  bool effects_runtime;
  bool json_control;
  bool query;
  bool actions;
  bool config_writes;
  bool watch;
};

const struct leme_public_schema *
leme_public_root_schema(enum leme_public_root root);
const struct leme_public_schema *
leme_public_entity_schema(enum leme_public_entity entity);
enum leme_public_status
leme_public_schema_validate(const struct leme_public_schema *schema,
                            const struct leme_public_value *value);

const struct leme_public_schema *
leme_public_field_schema(const struct leme_public_field *field);
bool leme_public_field_nullable(const struct leme_public_field *field);
struct leme_public_text
leme_public_field_name(const struct leme_public_field *field);
enum leme_public_kind
leme_public_schema_kind(const struct leme_public_schema *schema);
const struct leme_public_schema *
leme_public_schema_item(const struct leme_public_schema *schema);
const struct leme_public_schema *
leme_public_schema_reference(const struct leme_public_schema *schema);
size_t leme_public_schema_fields(const struct leme_public_schema *schema);
const struct leme_public_field *
leme_public_schema_field(const struct leme_public_schema *schema, size_t index);
bool leme_public_schema_entity(const struct leme_public_schema *schema,
                               enum leme_public_entity *out);

#endif
