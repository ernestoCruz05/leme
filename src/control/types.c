#include "control/types.h"

struct leme_control_type leme_control_type_any(void) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_ANY};
}

struct leme_control_type leme_control_type_null(void) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_NULL,
                                    .nullable = true};
}

struct leme_control_type leme_control_type_boolean(void) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_BOOLEAN};
}

struct leme_control_type leme_control_type_number(void) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_NUMBER};
}

struct leme_control_type leme_control_type_string(void) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_STRING};
}

struct leme_control_type
leme_control_type_entity(enum leme_public_entity entity,
                         enum leme_control_provenance provenance) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_ENTITY,
                                    .entity = entity,
                                    .provenance = provenance,
                                    .schema =
                                        leme_public_entity_schema(entity)};
}

struct leme_control_type
leme_control_type_record(const struct leme_public_schema *schema) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_RECORD,
                                    .schema = schema};
}

struct leme_control_type
leme_control_type_array(const struct leme_control_type *item) {
  return (struct leme_control_type){.kind = LEME_CONTROL_TYPE_ARRAY,
                                    .item_type = item};
}

bool leme_control_type_equal(const struct leme_control_type *left,
                             const struct leme_control_type *right) {
  if (left == NULL || right == NULL)
    return left == right;
  if (left->kind != right->kind || left->nullable != right->nullable)
    return false;
  if (left->kind == LEME_CONTROL_TYPE_ENTITY)
    return left->entity == right->entity &&
           left->provenance == right->provenance;
  if (left->kind == LEME_CONTROL_TYPE_RECORD)
    return left->schema == right->schema;
  if (left->kind == LEME_CONTROL_TYPE_ARRAY)
    return leme_control_type_equal(left->item_type, right->item_type);
  return true;
}

bool leme_control_type_compatible(const struct leme_control_type *expected,
                                  const struct leme_control_type *actual) {
  if (expected == NULL || actual == NULL)
    return false;
  if (expected->kind == LEME_CONTROL_TYPE_ANY)
    return true;
  if (actual->kind == LEME_CONTROL_TYPE_NULL)
    return expected->nullable;
  if (expected->kind == LEME_CONTROL_TYPE_ARRAY &&
      actual->kind == LEME_CONTROL_TYPE_ARRAY) {
    if (expected->item_type == NULL ||
        expected->item_type->kind == LEME_CONTROL_TYPE_ANY)
      return true;
    if (actual->item_type == NULL)
      return true;
    return leme_control_type_compatible(expected->item_type, actual->item_type);
  }
  if (expected->kind == LEME_CONTROL_TYPE_ENTITY &&
      actual->kind == LEME_CONTROL_TYPE_ENTITY)
    return expected->entity == actual->entity;
  if (expected->kind == LEME_CONTROL_TYPE_RECORD &&
      actual->kind == LEME_CONTROL_TYPE_RECORD)
    return expected->schema == NULL || expected->schema == actual->schema;
  return expected->kind == actual->kind;
}

bool leme_control_type_is_trusted_entity(const struct leme_control_type *type) {
  return type != NULL && type->kind == LEME_CONTROL_TYPE_ENTITY &&
         type->provenance == LEME_CONTROL_PROVENANCE_SNAPSHOT;
}

static struct leme_control_type entity_types[4][3];
static struct leme_control_type collection_types[4][3];
static bool types_initialized = false;

static void init_types(void) {
  if (types_initialized)
    return;
  for (int e = 0; e < 4; ++e) {
    for (int p = 0; p < 3; ++p) {
      entity_types[e][p] = (struct leme_control_type){
          .kind = LEME_CONTROL_TYPE_ENTITY,
          .entity = (enum leme_public_entity)e,
          .provenance = (enum leme_control_provenance)p,
          .schema = leme_public_entity_schema((enum leme_public_entity)e),
      };
      collection_types[e][p] = (struct leme_control_type){
          .kind = LEME_CONTROL_TYPE_ARRAY,
          .item_type = &entity_types[e][p],
          .provenance = (enum leme_control_provenance)p,
      };
    }
  }
  types_initialized = true;
}

const struct leme_control_type *
leme_control_type_entity_ref(enum leme_public_entity entity,
                             enum leme_control_provenance provenance) {
  if ((size_t)entity >= 4 || (size_t)provenance >= 3)
    return NULL;
  init_types();
  return &entity_types[entity][provenance];
}

const struct leme_control_type *
leme_control_type_collection_ref(enum leme_public_entity entity,
                                 enum leme_control_provenance provenance) {
  if ((size_t)entity >= 4 || (size_t)provenance >= 3)
    return NULL;
  init_types();
  return &collection_types[entity][provenance];
}

struct leme_control_type
leme_control_type_from_value(const struct leme_public_value *value) {
  if (value == NULL)
    return leme_control_type_null();
  enum leme_public_kind kind = leme_public_kind(value);
  switch (kind) {
  case LEME_PUBLIC_NULL:
    return leme_control_type_null();
  case LEME_PUBLIC_BOOLEAN:
    return leme_control_type_boolean();
  case LEME_PUBLIC_NUMBER:
    return leme_control_type_number();
  case LEME_PUBLIC_STRING:
    return leme_control_type_string();
  case LEME_PUBLIC_ARRAY: {
    static const struct leme_control_type any_t = {.kind =
                                                       LEME_CONTROL_TYPE_ANY};
    return leme_control_type_array(&any_t);
  }
  case LEME_PUBLIC_OBJECT:
    return (struct leme_control_type){
        .kind = LEME_CONTROL_TYPE_RECORD,
        .provenance = LEME_CONTROL_PROVENANCE_SYNTHETIC,
    };
  }
  return leme_control_type_any();
}
