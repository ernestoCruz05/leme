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
  [LEME_SCHEMA_##                                                              \
      tag] = {.name = "string",                                                \
              .kind = LEME_PUBLIC_STRING,                                      \
              .enum_values = enum_##tag,                                       \
              .enum_count = sizeof(enum_##tag) / sizeof(enum_##tag[0])},
#define PUBLIC_SCHEMA(tag, label, is_sparse, ...)                              \
  [LEME_SCHEMA_##                                                              \
      tag] = {.name = (label),                                                 \
              .kind = LEME_PUBLIC_OBJECT,                                      \
              .sparse = (is_sparse),                                           \
              .fields = fields_##tag,                                          \
              .field_count = sizeof(fields_##tag) / sizeof(fields_##tag[0])},
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
static const struct leme_public_registry registry = {
    .types = types + 1,
    .type_count = LEME_SCHEMA_TYPE_COUNT - 1u,
    .roots = roots,
    .root_count = sizeof(roots) / sizeof(roots[0])};

const struct leme_public_registry *leme_public_registry(void) {
  return &registry;
}
enum leme_public_status
leme_public_registry_validate(const struct leme_public_registry *input) {
  if (input == NULL || input->types == NULL || input->type_count == 0 ||
      (input->root_count != 0 && input->roots == NULL))
    return LEME_PUBLIC_INVALID;
  if (input->type_count > LEME_SCHEMA_TYPE_COUNT ||
      input->root_count > LEME_PUBLIC_ROOT_COUNT)
    return LEME_PUBLIC_LIMIT;
  for (size_t i = 0; i < input->type_count; ++i) {
    const struct leme_public_schema *schema = &input->types[i];
    if (schema->name == NULL || schema->name[0] == '\0' ||
        (schema->field_count != 0 && schema->fields == NULL) ||
        (schema->enum_count != 0 && schema->enum_values == NULL))
      return LEME_PUBLIC_INVALID;
    if (schema->field_count > 256 || schema->enum_count > 256)
      return LEME_PUBLIC_LIMIT;
    if (schema->kind == LEME_PUBLIC_ARRAY && schema->items == NULL)
      return LEME_PUBLIC_INVALID;
    if ((schema->primary == NULL) != (schema->alternative == NULL))
      return LEME_PUBLIC_INVALID;
    if ((schema->has_minimum && !isfinite(schema->minimum)) ||
        (schema->has_maximum && !isfinite(schema->maximum)) ||
        (schema->has_minimum && schema->has_maximum &&
         schema->minimum > schema->maximum))
      return LEME_PUBLIC_INVALID;
    for (size_t j = 0; j < schema->field_count; ++j) {
      const struct leme_public_field *field = &schema->fields[j];
      if (field->name.data == NULL || field->name.length == 0 ||
          field->type == NULL || field->description == NULL ||
          field->description[0] == '\0' ||
          (!schema->sparse && !field->required))
        return LEME_PUBLIC_INVALID;
      for (size_t k = 0; k < j; ++k) {
        const struct leme_public_text earlier = schema->fields[k].name;
        if (earlier.length == field->name.length &&
            memcmp(earlier.data, field->name.data, earlier.length) == 0)
          return LEME_PUBLIC_INVALID;
      }
    }
    for (size_t j = 0; j < schema->enum_count; ++j) {
      if (schema->enum_values[j] == NULL)
        return LEME_PUBLIC_INVALID;
      for (size_t k = 0; k < j; ++k)
        if (strcmp(schema->enum_values[j], schema->enum_values[k]) == 0)
          return LEME_PUBLIC_INVALID;
    }
    if (schema->fields != NULL && schema->related == NULL) {
      for (size_t j = 0; j < i; ++j) {
        const struct leme_public_schema *earlier = &input->types[j];
        if (earlier->fields != NULL && earlier->related == NULL &&
            strcmp(earlier->name, schema->name) == 0)
          return LEME_PUBLIC_INVALID;
      }
    }
  }
  for (size_t i = 0; i < input->root_count; ++i) {
    if (input->roots[i].name == NULL || input->roots[i].type == NULL)
      return LEME_PUBLIC_INVALID;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(input->roots[i].name, input->roots[j].name) == 0)
        return LEME_PUBLIC_INVALID;
  }
  return LEME_PUBLIC_OK;
}
const struct leme_public_schema *
leme_public_root_schema(enum leme_public_root root) {
  return (size_t)root < registry.root_count ? roots[root].type : NULL;
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

enum leme_public_status
leme_public_schema_path(const struct leme_public_schema *schema,
                        const struct leme_public_text *path, size_t count,
                        const struct leme_public_field **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (count > LEME_PUBLIC_MAX_FIELD_DEPTH)
    return LEME_PUBLIC_LIMIT;
  if (schema == NULL || path == NULL || count == 0)
    return LEME_PUBLIC_INVALID;
  const struct leme_public_field *field = NULL;
  for (size_t i = 0; i < count; ++i) {
    if (schema->related != NULL)
      schema = schema->related;
    if (schema->kind == LEME_PUBLIC_ARRAY)
      schema = schema->items;
    if (schema == NULL || schema->kind != LEME_PUBLIC_OBJECT)
      return LEME_PUBLIC_TYPE_ERROR;
    field = find_field(schema, path[i]);
    if (field == NULL)
      return LEME_PUBLIC_UNKNOWN_FIELD;
    schema = field->type;
  }
  *out = field;
  return LEME_PUBLIC_OK;
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
static const char *kind_name(const struct leme_public_schema *schema) {
  if (schema->primary != NULL)
    return "union";
  if (schema->rule == LEME_RULE_ANY)
    return "any";
  switch (schema->kind) {
  case LEME_PUBLIC_NULL:
    return "null";
  case LEME_PUBLIC_BOOLEAN:
    return "boolean";
  case LEME_PUBLIC_NUMBER:
    return schema->integer ? "integer" : "number";
  case LEME_PUBLIC_STRING:
    return "string";
  case LEME_PUBLIC_ARRAY:
    return "array";
  case LEME_PUBLIC_OBJECT:
    return "object";
  }
  return "invalid";
}

static const char *access_name(enum leme_public_access access) {
  return access == LEME_ACCESS_SAFE ? "safe" : "sensitive";
}

static enum leme_public_status array_text(struct leme_public_builder *b,
                                          struct leme_public_value *array,
                                          size_t index,
                                          struct leme_public_text text) {
  struct leme_public_value *value = NULL;
  enum leme_public_status status = leme_public_string(b, text, false, &value);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_array_set(b, array, index, value);
  return status;
}

static enum leme_public_status array_cstr(struct leme_public_builder *b,
                                          struct leme_public_value *array,
                                          size_t index, const char *text) {
  return array_text(
      b, array, index,
      (struct leme_public_text){.data = text, .length = strlen(text)});
}

static enum leme_public_status
field_relation(struct leme_public_builder *b, struct leme_public_value *record,
               const struct leme_public_schema *schema) {
  if (schema->related != NULL)
    return leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("relation"),
                                schema->related->name);
  if (schema->primary != NULL && schema->alternative != NULL &&
      schema->primary->related != NULL &&
      schema->alternative->related != NULL) {
    struct leme_public_value *array = NULL;
    if (leme_public_array(b, 2, &array) != LEME_PUBLIC_OK ||
        array_cstr(b, array, 0, schema->primary->related->name) !=
            LEME_PUBLIC_OK ||
        array_cstr(b, array, 1, schema->alternative->related->name) !=
            LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    return leme_public_object_set(b, record, LEME_PUBLIC_TEXT("relation"),
                                  array);
  }
  return leme_public_put_null(b, record, LEME_PUBLIC_TEXT("relation"));
}

static enum leme_public_status
field_value(struct leme_public_builder *b,
            const struct leme_public_field *field,
            const struct leme_public_features *features,
            const struct leme_public_text *path, size_t path_count,
            struct leme_public_value **out) {
  *out = NULL;
  struct leme_public_value *record = NULL;
  struct leme_public_value *enums = NULL;
  const struct leme_public_schema *schema = field->type;
  const bool available =
      field->condition != LEME_CONDITION_EFFECTS || features->effects_build;
  const bool known = field->condition != LEME_CONDITION_EFFECTS || !available ||
                     features->effects_runtime_known;
  const bool effective = field->condition == LEME_CONDITION_ALWAYS ||
                         (field->condition == LEME_CONDITION_EFFECTS &&
                          available && features->effects_runtime);
  const struct leme_public_schema *first =
      schema->primary != NULL ? schema->primary : schema;
  const struct leme_public_schema *second = schema->alternative;
  const char *items = first->kind == LEME_PUBLIC_ARRAY ? first->items->name
                      : second != NULL && second->kind == LEME_PUBLIC_ARRAY
                          ? second->items->name
                          : NULL;
  const char *properties =
      first->kind == LEME_PUBLIC_OBJECT                      ? first->name
      : second != NULL && second->kind == LEME_PUBLIC_OBJECT ? second->name
                                                             : NULL;
  const bool writable =
      field->write_capability != NULL && available &&
      strcmp(field->write_capability, "config_writes") == 0 &&
      features->config_writes;
  if (leme_public_object(b, path == NULL ? 18u : 19u, &record) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("name"), field->name,
                           false) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"),
                           kind_name(schema)) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("nullable"),
                           field->nullable) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("required"),
                           field->required) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("units"),
                           field->units) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("description"),
                           field->description) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("access"),
                           access_name(field->access)) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("available"),
                           available) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("writable"),
                           writable) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("write_capability"),
                           field->write_capability) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("items"), items) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("properties"),
                           properties) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("stability"),
                           "api-versioned") != LEME_PUBLIC_OK ||
      field_relation(b, record, schema) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if ((known
           ? leme_public_put_bool(
                 b, record, LEME_PUBLIC_TEXT("effective_in_runtime"), effective)
           : leme_public_put_null(b, record,
                                  LEME_PUBLIC_TEXT("effective_in_runtime"))) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if ((schema->has_minimum
           ? leme_public_put_number(b, record, LEME_PUBLIC_TEXT("minimum"),
                                    schema->minimum)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("minimum"))) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if ((schema->has_maximum
           ? leme_public_put_number(b, record, LEME_PUBLIC_TEXT("maximum"),
                                    schema->maximum)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("maximum"))) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const size_t enum_count =
      schema->rule == LEME_RULE_TRUE ? 1u : schema->enum_count;
  if (leme_public_array(b, enum_count, &enums) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (schema->rule == LEME_RULE_TRUE) {
    struct leme_public_value *value = NULL;
    if (leme_public_boolean(b, true, &value) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, enums, 0, value) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else {
    for (size_t i = 0; i < schema->enum_count; ++i)
      if (array_cstr(b, enums, i, schema->enum_values[i]) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("enum"), enums) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (path != NULL) {
    struct leme_public_value *segments = NULL;
    if (leme_public_array(b, path_count, &segments) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    for (size_t i = 0; i < path_count; ++i) {
      if (array_text(b, segments, i, path[i]) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
    if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("path"), segments) !=
        LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
count_settings(const struct leme_public_schema *schema,
               const struct leme_public_value *value, size_t depth,
               size_t *count, size_t *remaining) {
  if (value == NULL || leme_public_kind(value) != LEME_PUBLIC_OBJECT ||
      schema->kind != LEME_PUBLIC_OBJECT)
    return LEME_PUBLIC_TYPE_ERROR;
  if (depth >= LEME_PUBLIC_MAX_FIELD_DEPTH && leme_public_length(value) != 0)
    return LEME_PUBLIC_LIMIT;
  if (*remaining == 0)
    return LEME_PUBLIC_LIMIT;
  --*remaining;
  for (size_t i = 0; i < leme_public_length(value); ++i) {
    const struct leme_public_text key = leme_public_key_at(value, i);
    if (key.length > *remaining || *count == SIZE_MAX)
      return LEME_PUBLIC_LIMIT;
    *remaining -= key.length;
    ++*count;
    const struct leme_public_field *field = find_field(schema, key);
    const struct leme_public_value *member = leme_public_member_at(value, i);
    if (field == NULL)
      return LEME_PUBLIC_UNKNOWN_FIELD;
    if (member == NULL)
      return LEME_PUBLIC_INVALID;
    if (leme_public_kind(member) == LEME_PUBLIC_NULL && field->nullable)
      continue;
    const enum leme_public_status status =
        field->type->primary == NULL && field->type->kind == LEME_PUBLIC_OBJECT
            ? count_settings(field->type, member, depth + 1, count, remaining)
            : validate_value(field->type, member, depth + 1, remaining);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_settings_validate(const struct leme_public_value *settings) {
  if (settings == NULL)
    return LEME_PUBLIC_INVALID;
  size_t count = 0, remaining = settings->owner->maximum;
  return count_settings(&types[LEME_SCHEMA_SETTINGS], settings, 0, &count,
                        &remaining);
}

struct settings_metadata {
  struct leme_public_builder *builder;
  const struct leme_public_features *features;
  struct leme_public_value *array;
  struct leme_public_text path[LEME_PUBLIC_MAX_FIELD_DEPTH];
  size_t count;
};

static enum leme_public_status
collect_settings(struct settings_metadata *metadata,
                 const struct leme_public_schema *schema,
                 const struct leme_public_value *value, size_t depth) {
  struct leme_public_builder *b = metadata->builder;
  if (depth >= LEME_PUBLIC_MAX_FIELD_DEPTH && leme_public_length(value) != 0)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  for (size_t i = 0; i < schema->field_count; ++i) {
    const struct leme_public_field *field = &schema->fields[i];
    const struct leme_public_value *member =
        leme_public_get(value, field->name);
    if (member == NULL)
      continue;
    if (depth >= LEME_PUBLIC_MAX_FIELD_DEPTH)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    metadata->path[depth] = field->name;
    struct leme_public_value *row = NULL;
    if (field_value(b, field, metadata->features, metadata->path, depth + 1,
                    &row) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, metadata->array, metadata->count++, row) !=
            LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    if (leme_public_kind(member) == LEME_PUBLIC_OBJECT &&
        field->type->primary == NULL &&
        field->type->kind == LEME_PUBLIC_OBJECT &&
        collect_settings(metadata, field->type, member, depth + 1) !=
            LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_settings_schema_value(struct leme_public_builder *b,
                                  const struct leme_public_value *settings,
                                  const struct leme_public_features *features,
                                  struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (settings == NULL || features == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  size_t count = 0, remaining = settings->owner->maximum;
  const enum leme_public_status status = count_settings(
      &types[LEME_SCHEMA_SETTINGS], settings, 0, &count, &remaining);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  struct settings_metadata metadata = {
      .builder = b, .features = features, .array = array};
  if (collect_settings(&metadata, &types[LEME_SCHEMA_SETTINGS], settings, 0) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (metadata.count != count)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
root_value(struct leme_public_builder *b,
           const struct leme_public_root_descriptor *root,
           struct leme_public_value **out) {
  *out = NULL;
  struct leme_public_value *record = NULL;
  const bool collection = root->type->kind == LEME_PUBLIC_ARRAY;
  const char *type = collection ? root->type->items->name : root->type->name;
  if (leme_public_object(b, 4, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), root->name) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"), type) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("collection"),
                           collection) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("access"),
                           access_name(root->access)) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
type_value(struct leme_public_builder *b,
           const struct leme_public_schema *schema,
           const struct leme_public_features *features,
           struct leme_public_value **out) {
  *out = NULL;
  struct leme_public_value *record = NULL;
  struct leme_public_value *fields = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), schema->name) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("sparse"),
                           schema->sparse) != LEME_PUBLIC_OK ||
      leme_public_array(b, schema->field_count, &fields) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < schema->field_count; ++i) {
    struct leme_public_value *field = NULL;
    if (field_value(b, &schema->fields[i], features, NULL, 0, &field) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, fields, i, field) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("fields"), fields) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_root_describe(struct leme_public_builder *b,
                          struct leme_public_text name,
                          struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (b == NULL || name.data == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  for (size_t i = 0; i < registry.root_count; ++i) {
    const struct leme_public_root_descriptor *root = &registry.roots[i];
    if (name.length == strlen(root->name) &&
        memcmp(name.data, root->name, name.length) == 0) {
      return root_value(b, root, out);
    }
  }
  return LEME_PUBLIC_NOT_FOUND;
}

enum leme_public_status
leme_public_type_describe(struct leme_public_builder *b,
                          struct leme_public_text name,
                          const struct leme_public_features *features,
                          struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (b == NULL || name.data == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  static const struct leme_public_features default_features = {0};
  if (features == NULL)
    features = &default_features;
  for (size_t i = 0; i < registry.type_count; ++i) {
    const struct leme_public_schema *schema = &registry.types[i];
    if (schema->fields == NULL || schema->related != NULL)
      continue;
    if (name.length == strlen(schema->name) &&
        memcmp(name.data, schema->name, name.length) == 0) {
      return type_value(b, schema, features, out);
    }
  }
  return LEME_PUBLIC_NOT_FOUND;
}

enum leme_public_status
leme_public_operation_value(struct leme_public_builder *b,
                            const struct leme_public_operation *operation,
                            struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (b == NULL || operation == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL;
  struct leme_public_value *arguments = NULL;
  if (leme_public_object(b, 10, &record) != LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("name"), operation->name,
                           false) != LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("description"),
                           operation->documentation, false) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("effect"),
                           operation->action ? "action" : "pure") !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("available"),
                           operation->available) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (operation->stability.data != NULL && operation->stability.length != 0) {
    if (leme_public_put_text(b, record, LEME_PUBLIC_TEXT("stability"),
                             operation->stability, false) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else {
    if (leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("stability"),
                             "api-versioned") != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_put_number(b, record, LEME_PUBLIC_TEXT("min_args"),
                             (double)operation->min_args) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("max_args"),
                             (double)operation->max_args) != LEME_PUBLIC_OK ||
      leme_public_array(b, operation->argument_count, &arguments) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < operation->argument_count; ++i) {
    struct leme_public_value *arg = NULL;
    if (leme_public_object(b, 2, &arg) != LEME_PUBLIC_OK ||
        leme_public_put_text(b, arg, LEME_PUBLIC_TEXT("type"),
                             operation->arguments[i].type, false) !=
            LEME_PUBLIC_OK ||
        leme_public_put_text(b, arg, LEME_PUBLIC_TEXT("scope"),
                             operation->arguments[i].scope, false) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, arguments, i, arg) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("arguments"),
                             arguments) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (operation->result.data != NULL && operation->result.length != 0) {
    if (leme_public_put_text(b, record, LEME_PUBLIC_TEXT("result"),
                             operation->result, false) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else {
    if (leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("result"), "any") !=
        LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (operation->work.data != NULL && operation->work.length != 0) {
    if (leme_public_put_text(b, record, LEME_PUBLIC_TEXT("work"),
                             operation->work, false) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else {
    if (leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("work"), "") !=
        LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
operation_value(struct leme_public_builder *b,
                const struct leme_public_operation *operation,
                struct leme_public_value **out) {
  return leme_public_operation_value(b, operation, out);
}

static int compare_operations(const void *lhs, const void *rhs) {
  const struct leme_public_operation *left = lhs;
  const struct leme_public_operation *right = rhs;
  const size_t a = left->name.length;
  const size_t b = right->name.length;
  const int result = memcmp(left->name.data, right->name.data, a < b ? a : b);
  return result != 0 ? result : (a > b) - (a < b);
}

enum leme_public_status leme_public_schema_value(
    struct leme_public_builder *b, const struct leme_public_features *features,
    const struct leme_public_operation *operations, size_t operation_count,
    struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (b == NULL || features == NULL ||
      (operation_count != 0 && operations == NULL))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (operation_count > b->maximum / sizeof(*operations))
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  size_t remaining = b->maximum;
  for (size_t i = 0; i < operation_count; ++i) {
    const struct leme_public_operation *op = &operations[i];
    if (op->name.length == 0 || op->name.data == NULL ||
        (op->documentation.data == NULL && op->documentation.length != 0) ||
        (op->available && !(op->action ? features->actions : features->query)))
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (op->name.length > remaining)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    remaining -= op->name.length;
    if (op->documentation.length > remaining)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    remaining -= op->documentation.length;
  }
  struct leme_public_operation *sorted = NULL;
  if (operation_count != 0) {
    sorted = leme_public_allocate(b, operation_count, sizeof(*sorted),
                                  alignof(struct leme_public_operation));
    if (sorted == NULL)
      return leme_public_builder_status(b);
    for (size_t i = 0; i < operation_count; ++i)
      sorted[i] = operations[i];
    qsort(sorted, operation_count, sizeof(*sorted), compare_operations);
    for (size_t i = 1; i < operation_count; ++i)
      if (compare_operations(&sorted[i - 1], &sorted[i]) == 0)
        return leme_public_fail(b, LEME_PUBLIC_INVALID);
  }
  status = leme_public_registry_validate(&registry);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  size_t type_count = 0;
  for (size_t i = 0; i < registry.type_count; ++i) {
    const struct leme_public_schema *schema = &registry.types[i];
    if (schema->fields != NULL && schema->related == NULL)
      ++type_count;
  }
  struct leme_public_value *record = NULL;
  struct leme_public_value *root_array = NULL;
  struct leme_public_value *type_array = NULL;
  struct leme_public_value *operators = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      leme_public_array(b, registry.root_count, &root_array) !=
          LEME_PUBLIC_OK ||
      leme_public_array(b, type_count, &type_array) != LEME_PUBLIC_OK ||
      leme_public_array(b, operation_count, &operators) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < registry.root_count; ++i) {
    struct leme_public_value *root = NULL;
    if (root_value(b, &registry.roots[i], &root) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, root_array, i, root) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  size_t index = 0;
  for (size_t i = 0; i < registry.type_count; ++i) {
    const struct leme_public_schema *schema = &registry.types[i];
    if (schema->fields == NULL || schema->related != NULL)
      continue;
    struct leme_public_value *type = NULL;
    if (type_value(b, schema, features, &type) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, type_array, index, type) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    ++index;
  }
  for (size_t i = 0; i < operation_count; ++i) {
    struct leme_public_value *operation = NULL;
    if (operation_value(b, &sorted[i], &operation) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, operators, i, operation) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("roots"),
                             root_array) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("types"),
                             type_array) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("operators"),
                             operators) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
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

bool leme_public_schema_any(const struct leme_public_schema *schema) {
  return schema != NULL && schema->rule == LEME_RULE_ANY;
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

size_t leme_public_schema_alternatives(const struct leme_public_schema *schema) {
  if (schema == NULL)
    return 0;
  if (schema->primary != NULL && schema->alternative != NULL)
    return 2;
  if (schema->primary != NULL)
    return 1;
  return 0;
}

const struct leme_public_schema *
leme_public_schema_alternative(const struct leme_public_schema *schema,
                               size_t index) {
  if (schema == NULL)
    return NULL;
  if (index == 0)
    return schema->primary;
  if (index == 1)
    return schema->alternative;
  return NULL;
}

size_t leme_public_schema_fields(const struct leme_public_schema *schema) {
  return schema != NULL ? schema->field_count : 0;
}

const struct leme_public_field *
leme_public_schema_field(const struct leme_public_schema *schema, size_t index) {
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
