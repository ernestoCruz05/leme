#include "public/schema-internal.h"

#include "public/value-internal.h"

#include <float.h>
#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

static const struct leme_public_schema types[LEME_SCHEMA_TYPE_COUNT];

#define PUBLIC_SCALAR(...)
#define PUBLIC_REFERENCE(...)
#define PUBLIC_ARRAY(...)
#define PUBLIC_NUMBER(...)
#define PUBLIC_UNION(...)
#define PUBLIC_ENUM(tag, ...)                                                  \
  static const char *const enum_##tag[] = {__VA_ARGS__};
#define PUBLIC_SCHEMA(tag, label, is_sparse, ...)                              \
  static const struct leme_public_field fields_##tag[] = {__VA_ARGS__};
#define PUBLIC_FIELD(key, field_type, maybe_null, is_required, field_units,    \
                     field_access, feature, capability, help)                  \
  {.name = {.data = (key), .length = sizeof(key) - 1u},                        \
   .type = &types[LEME_SCHEMA_##field_type],                                   \
   .nullable = (maybe_null),                                                   \
   .required = (is_required),                                                  \
   .units = (field_units),                                                     \
   .access = LEME_ACCESS_##field_access,                                       \
   .condition = LEME_CONDITION_##feature,                                      \
   .write_capability = (capability),                                           \
   .description = (help)}
#include "public/schema.def"
#undef PUBLIC_SCALAR
#undef PUBLIC_REFERENCE
#undef PUBLIC_ARRAY
#undef PUBLIC_NUMBER
#undef PUBLIC_UNION
#undef PUBLIC_ENUM
#undef PUBLIC_SCHEMA
#undef PUBLIC_FIELD

static const struct leme_public_schema types[LEME_SCHEMA_TYPE_COUNT] = {
#define PUBLIC_SCALAR(tag, label, value_kind, value_rule)                      \
  [LEME_SCHEMA_##tag] = {                                                      \
      .name = (label), .kind = (value_kind), .rule = LEME_RULE_##value_rule},
#define PUBLIC_REFERENCE(tag, target)                                          \
  [LEME_SCHEMA_##tag] = {.name = "reference",                                  \
                         .kind = LEME_PUBLIC_OBJECT,                           \
                         .related = &types[LEME_SCHEMA_##target],              \
                         .fields = fields_REFERENCE,                           \
                         .field_count = sizeof(fields_REFERENCE) /             \
                                        sizeof(fields_REFERENCE[0])},
#define PUBLIC_ARRAY(tag, element)                                             \
  [LEME_SCHEMA_##tag] = {.name = "array",                                      \
                         .kind = LEME_PUBLIC_ARRAY,                            \
                         .items = &types[LEME_SCHEMA_##element]},
#define PUBLIC_NUMBER(tag, is_integer, has_low, low, has_high, high,           \
                      value_rule)                                              \
  [LEME_SCHEMA_##tag] = {.name = (is_integer) ? "integer" : "number",          \
                         .kind = LEME_PUBLIC_NUMBER,                           \
                         .integer = (is_integer),                              \
                         .has_minimum = (has_low),                             \
                         .minimum = (low),                                     \
                         .has_maximum = (has_high),                            \
                         .maximum = (high),                                    \
                         .rule = LEME_RULE_##value_rule},
#define PUBLIC_ENUM(tag, ...)                                                  \
  [LEME_SCHEMA_##tag] = {.name = "string",                                     \
                         .kind = LEME_PUBLIC_STRING,                           \
                         .enum_values = enum_##tag,                            \
                         .enum_count =                                         \
                             sizeof(enum_##tag) / sizeof(enum_##tag[0])},
#define PUBLIC_SCHEMA(tag, label, is_sparse, ...)                              \
  [LEME_SCHEMA_##tag] = {.name = (label),                                      \
                         .kind = LEME_PUBLIC_OBJECT,                           \
                         .sparse = (is_sparse),                                \
                         .fields = fields_##tag,                               \
                         .field_count =                                        \
                             sizeof(fields_##tag) / sizeof(fields_##tag[0])},
#define PUBLIC_UNION(tag, first, second)                                       \
  [LEME_SCHEMA_##tag] = {.name = "union",                                      \
                         .primary = &types[LEME_SCHEMA_##first],               \
                         .alternative = &types[LEME_SCHEMA_##second]},
#include "public/schema.def"
#undef PUBLIC_SCALAR
#undef PUBLIC_REFERENCE
#undef PUBLIC_ARRAY
#undef PUBLIC_NUMBER
#undef PUBLIC_UNION
#undef PUBLIC_ENUM
#undef PUBLIC_SCHEMA
};

static const struct leme_public_root_descriptor roots[] = {
    {"views", &types[LEME_SCHEMA_VIEWS], LEME_ACCESS_SENSITIVE},
    {"tags", &types[LEME_SCHEMA_TAGS], LEME_ACCESS_SENSITIVE},
    {"outputs", &types[LEME_SCHEMA_OUTPUTS], LEME_ACCESS_SENSITIVE},
    {"inputs", &types[LEME_SCHEMA_INPUTS], LEME_ACCESS_SENSITIVE},
    {"session", &types[LEME_SCHEMA_SESSION], LEME_ACCESS_SENSITIVE},
    {"config", &types[LEME_SCHEMA_CONFIG], LEME_ACCESS_SENSITIVE},
    {"runtime", &types[LEME_SCHEMA_RUNTIME], LEME_ACCESS_SAFE},
    {"status", &types[LEME_SCHEMA_STATUS], LEME_ACCESS_SAFE}};
const struct leme_public_schema *
leme_public_root_schema(enum leme_public_root root) {
  return (size_t)root < sizeof(roots) / sizeof(roots[0]) ? roots[root].type
                                                         : NULL;
}
const struct leme_public_schema *
leme_public_entity_schema(enum leme_public_entity entity) {
  switch (entity) {
  case LEME_PUBLIC_VIEW:
    return &types[LEME_SCHEMA_VIEW];
  case LEME_PUBLIC_OUTPUT:
    return &types[LEME_SCHEMA_OUTPUT];
  case LEME_PUBLIC_TAG:
    return &types[LEME_SCHEMA_TAG];
  case LEME_PUBLIC_INPUT:
    return &types[LEME_SCHEMA_INPUT];
  }
  return NULL;
}

static const struct leme_public_field *
find_field(const struct leme_public_schema *schema,
           struct leme_public_text key) {
  if (key.data == NULL && key.length != 0)
    return NULL;
  for (size_t i = 0; i < schema->field_count; ++i) {
    const struct leme_public_field *field = &schema->fields[i];
    if (key.length == field->name.length &&
        (key.length == 0 ||
         memcmp(key.data, field->name.data, key.length) == 0))
      return field;
  }
  return NULL;
}

static bool text_is(struct leme_public_text text, const char *expected) {
  const size_t length = strlen(expected);
  return text.length == length &&
         (length == 0 || memcmp(text.data, expected, length) == 0);
}

static enum leme_public_status
validate_value(const struct leme_public_schema *schema,
               const struct leme_public_value *value, size_t depth,
               size_t *remaining) {
  if (schema == NULL || value == NULL)
    return LEME_PUBLIC_INVALID;
  if (depth > LEME_PUBLIC_MAX_DEPTH || *remaining == 0)
    return LEME_PUBLIC_LIMIT;
  --*remaining;
  if (schema->rule == LEME_RULE_ANY)
    return LEME_PUBLIC_OK;
  if (schema->primary != NULL) {
    enum leme_public_status status =
        validate_value(schema->primary, value, depth, remaining);
    if (status == LEME_PUBLIC_OK || status == LEME_PUBLIC_LIMIT)
      return status;
    status = validate_value(schema->alternative, value, depth, remaining);
    return status;
  }
  if (leme_public_kind(value) != schema->kind)
    return LEME_PUBLIC_TYPE_ERROR;
  switch (schema->kind) {
  case LEME_PUBLIC_NULL:
    return LEME_PUBLIC_OK;
  case LEME_PUBLIC_BOOLEAN: {
    bool result = false;
    const enum leme_public_status status = leme_public_as_bool(value, &result);
    if (status != LEME_PUBLIC_OK)
      return status;
    return schema->rule == LEME_RULE_TRUE && !result ? LEME_PUBLIC_INVALID
                                                     : LEME_PUBLIC_OK;
  }
  case LEME_PUBLIC_NUMBER: {
    double number = 0.0;
    const enum leme_public_status status =
        leme_public_as_number(value, &number);
    if (status != LEME_PUBLIC_OK)
      return status;
    if ((schema->integer && trunc(number) != number) ||
        (schema->has_minimum && number < schema->minimum) ||
        (schema->has_maximum && number > schema->maximum) ||
        (schema->rule == LEME_RULE_FINGERS && (number == 1.0 || number == 2.0)))
      return LEME_PUBLIC_INVALID;
    return LEME_PUBLIC_OK;
  }
  case LEME_PUBLIC_STRING: {
    struct leme_public_text text = {0};
    const enum leme_public_status status = leme_public_as_text(value, &text);
    if (status != LEME_PUBLIC_OK)
      return status;
    if (text.length > *remaining)
      return LEME_PUBLIC_LIMIT;
    *remaining -= text.length;
    if (schema->rule != LEME_RULE_NONE && text.length == 0)
      return LEME_PUBLIC_INVALID;
    if (schema->rule == LEME_RULE_COLOR) {
      if (text.length != 9 || text.data[0] != '#')
        return LEME_PUBLIC_INVALID;
      for (size_t i = 1; i < text.length; ++i) {
        const char c = text.data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
          return LEME_PUBLIC_INVALID;
      }
    }
    if (schema->rule == LEME_RULE_DECIMAL) {
      for (size_t i = 0; i < text.length; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
          return LEME_PUBLIC_INVALID;
    }
    if (schema->enum_count == 0)
      return LEME_PUBLIC_OK;
    for (size_t i = 0; i < schema->enum_count; ++i)
      if (text_is(text, schema->enum_values[i]))
        return LEME_PUBLIC_OK;
    return LEME_PUBLIC_INVALID;
  }
  case LEME_PUBLIC_ARRAY:
    for (size_t i = 0; i < leme_public_length(value); ++i) {
      const enum leme_public_status status = validate_value(
          schema->items, leme_public_at(value, i), depth + 1, remaining);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    return LEME_PUBLIC_OK;
  case LEME_PUBLIC_OBJECT:
    for (size_t i = 0; i < leme_public_length(value); ++i) {
      const struct leme_public_text key = leme_public_key_at(value, i);
      if (key.length > *remaining)
        return LEME_PUBLIC_LIMIT;
      *remaining -= key.length;
      const struct leme_public_field *field = find_field(schema, key);
      if (field == NULL)
        return LEME_PUBLIC_UNKNOWN_FIELD;
      const struct leme_public_value *member = leme_public_member_at(value, i);
      if (member == NULL)
        return LEME_PUBLIC_INVALID;
      if (leme_public_kind(member) == LEME_PUBLIC_NULL && field->nullable)
        continue;
      const enum leme_public_status status =
          validate_value(field->type, member, depth + 1, remaining);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    for (size_t i = 0; i < schema->field_count; ++i) {
      const struct leme_public_field *field = &schema->fields[i];
      if (field->required && leme_public_get(value, field->name) == NULL)
        return LEME_PUBLIC_INVALID;
    }
    if (schema->related != NULL) {
      struct leme_public_text kind = {0};
      if (leme_public_as_text(leme_public_get(value, LEME_PUBLIC_TEXT("type")),
                              &kind) != LEME_PUBLIC_OK ||
          !text_is(kind, schema->related->name))
        return LEME_PUBLIC_INVALID;
    }
    return LEME_PUBLIC_OK;
  }
  return LEME_PUBLIC_INVALID;
}

enum leme_public_status
leme_public_schema_validate(const struct leme_public_schema *schema,
                            const struct leme_public_value *value) {
  if (schema == NULL || value == NULL)
    return LEME_PUBLIC_INVALID;
  size_t remaining = value->owner->maximum;
  return validate_value(schema, value, 1, &remaining);
}
const struct leme_public_schema *
leme_public_field_schema(const struct leme_public_field *field) {
  return field != NULL ? field->type : NULL;
}

bool leme_public_field_nullable(const struct leme_public_field *field) {
  return field != NULL ? field->nullable : false;
}

struct leme_public_text
leme_public_field_name(const struct leme_public_field *field) {
  return field != NULL ? field->name : (struct leme_public_text){0};
}

enum leme_public_kind
leme_public_schema_kind(const struct leme_public_schema *schema) {
  return schema != NULL ? schema->kind : LEME_PUBLIC_NULL;
}

const struct leme_public_schema *
leme_public_schema_item(const struct leme_public_schema *schema) {
  return (schema != NULL && schema->kind == LEME_PUBLIC_ARRAY) ? schema->items
                                                               : NULL;
}

const struct leme_public_schema *
leme_public_schema_reference(const struct leme_public_schema *schema) {
  return schema != NULL ? schema->related : NULL;
}

size_t leme_public_schema_fields(const struct leme_public_schema *schema) {
  return schema != NULL ? schema->field_count : 0;
}

const struct leme_public_field *
leme_public_schema_field(const struct leme_public_schema *schema,
                         size_t index) {
  if (schema == NULL || index >= schema->field_count)
    return NULL;
  return &schema->fields[index];
}

bool leme_public_schema_entity(const struct leme_public_schema *schema,
                               enum leme_public_entity *out) {
  if (schema == NULL)
    return false;
  if (schema == &types[LEME_SCHEMA_VIEW]) {
    if (out != NULL)
      *out = LEME_PUBLIC_VIEW;
    return true;
  }
  if (schema == &types[LEME_SCHEMA_OUTPUT]) {
    if (out != NULL)
      *out = LEME_PUBLIC_OUTPUT;
    return true;
  }
  if (schema == &types[LEME_SCHEMA_TAG]) {
    if (out != NULL)
      *out = LEME_PUBLIC_TAG;
    return true;
  }
  if (schema == &types[LEME_SCHEMA_INPUT]) {
    if (out != NULL)
      *out = LEME_PUBLIC_INPUT;
    return true;
  }
  return false;
}
