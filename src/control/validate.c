#include "control/validate.h"

#include "control/types.h"
#include "public/model.h"
#include "public/schema.h"

#include <string.h>

const struct leme_public_field *
leme_control_schema_find_field(const struct leme_public_schema *schema,
                               struct leme_public_text name) {
  if (schema == NULL)
    return NULL;
  size_t count = leme_public_schema_fields(schema);
  for (size_t i = 0; i < count; ++i) {
    const struct leme_public_field *f = leme_public_schema_field(schema, i);
    struct leme_public_text fname = leme_public_field_name(f);
    if (fname.length == name.length &&
        memcmp(fname.data, name.data, name.length) == 0)
      return f;
  }
  return NULL;
}

const struct leme_public_schema *
leme_control_schema_follow_ref(const struct leme_public_schema *schema) {
  if (schema == NULL)
    return NULL;
  const struct leme_public_schema *ref = leme_public_schema_reference(schema);
  if (ref != NULL)
    return ref;
  return schema;
}

enum leme_control_code
leme_control_resolve_field_path(const struct leme_control_type *item_type,
                                const struct leme_public_text *components,
                                size_t count,
                                struct leme_control_type *out_type) {
  if (item_type == NULL || components == NULL || count == 0)
    return LEME_CONTROL_INVALID_ARGUMENT;
  if (item_type->kind != LEME_CONTROL_TYPE_ENTITY &&
      item_type->kind != LEME_CONTROL_TYPE_RECORD)
    return LEME_CONTROL_TYPE_ERROR;
  const struct leme_public_schema *schema = item_type->schema;
  if (schema == NULL && item_type->kind == LEME_CONTROL_TYPE_ENTITY)
    schema = leme_public_entity_schema(item_type->entity);
  if (schema == NULL) {
    if (item_type->kind == LEME_CONTROL_TYPE_RECORD) {
      if (out_type != NULL)
        *out_type = leme_control_type_any();
      return LEME_CONTROL_OK;
    }
    return LEME_CONTROL_TYPE_ERROR;
  }

  const struct leme_public_field *field = NULL;
  for (size_t i = 0; i < count; ++i) {
    field = leme_control_schema_find_field(schema, components[i]);
    if (field == NULL)
      return LEME_CONTROL_UNKNOWN_FIELD;
    const struct leme_public_schema *fschema = leme_public_field_schema(field);
    schema = leme_control_schema_follow_ref(fschema);
  }

  if (out_type != NULL && field != NULL) {
    const struct leme_public_schema *fschema = leme_public_field_schema(field);
    bool nullable = leme_public_field_nullable(field);
    enum leme_public_kind kind = leme_public_schema_kind(fschema);
    enum leme_public_entity ent;
    if (leme_public_schema_entity(schema, &ent)) {
      *out_type =
          leme_control_type_entity(ent, LEME_CONTROL_PROVENANCE_SNAPSHOT);
      out_type->nullable = nullable;
    } else {
      switch (kind) {
      case LEME_PUBLIC_NULL:
        *out_type = leme_control_type_null();
        break;
      case LEME_PUBLIC_BOOLEAN:
        *out_type = leme_control_type_boolean();
        out_type->nullable = nullable;
        break;
      case LEME_PUBLIC_NUMBER:
        *out_type = leme_control_type_number();
        out_type->nullable = nullable;
        break;
      case LEME_PUBLIC_STRING:
        *out_type = leme_control_type_string();
        out_type->nullable = nullable;
        break;
      case LEME_PUBLIC_ARRAY: {
        const struct leme_public_schema *item_schema =
            leme_public_schema_item(fschema);
        static struct leme_control_type item_t;
        item_t = (struct leme_control_type){.kind = LEME_CONTROL_TYPE_ANY};
        if (item_schema != NULL) {
          enum leme_public_entity item_ent;
          if (leme_public_schema_entity(item_schema, &item_ent))
            item_t = leme_control_type_entity(item_ent,
                                              LEME_CONTROL_PROVENANCE_SNAPSHOT);
          else
            item_t = leme_control_type_record(item_schema);
        }
        *out_type = leme_control_type_array(&item_t);
        out_type->nullable = nullable;
        break;
      }
      case LEME_PUBLIC_OBJECT:
        *out_type = leme_control_type_record(schema);
        out_type->nullable = nullable;
        break;
      }
    }
  }
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_control_infer_operator_result(const struct leme_control_operator *op,
                                   const struct leme_control_type *arg_types,
                                   size_t arg_count,
                                   struct leme_control_type *out_type) {
  if (op == NULL || out_type == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  switch (op->opcode) {
  case LEME_CONTROL_OP_VIEWS:
    *out_type = *leme_control_type_collection_ref(
        LEME_PUBLIC_VIEW, LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_TAGS:
    *out_type = *leme_control_type_collection_ref(
        LEME_PUBLIC_TAG, LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_OUTPUTS:
    *out_type = *leme_control_type_collection_ref(
        LEME_PUBLIC_OUTPUT, LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_INPUTS:
    *out_type = *leme_control_type_collection_ref(
        LEME_PUBLIC_INPUT, LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_SESSION:
    *out_type =
        leme_control_type_record(leme_public_root_schema(LEME_PUBLIC_SESSION));
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_CONFIG:
    *out_type =
        leme_control_type_record(leme_public_root_schema(LEME_PUBLIC_CONFIG));
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_RUNTIME:
    *out_type =
        leme_control_type_record(leme_public_root_schema(LEME_PUBLIC_RUNTIME));
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_STATUS:
    *out_type =
        leme_control_type_record(leme_public_root_schema(LEME_PUBLIC_STATUS));
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_VIEW:
    *out_type = *leme_control_type_entity_ref(LEME_PUBLIC_VIEW,
                                              LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_OUTPUT:
    *out_type = *leme_control_type_entity_ref(LEME_PUBLIC_OUTPUT,
                                              LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_TAG:
    *out_type = *leme_control_type_entity_ref(LEME_PUBLIC_TAG,
                                              LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_INPUT:
    *out_type = *leme_control_type_entity_ref(LEME_PUBLIC_INPUT,
                                              LEME_CONTROL_PROVENANCE_SNAPSHOT);
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_BY_ID:
    *out_type = leme_control_type_any();
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_WHERE:
  case LEME_CONTROL_OP_SORT_BY:
  case LEME_CONTROL_OP_LIMIT:
    if (arg_count > 0 && arg_types != NULL)
      *out_type = arg_types[0];
    else
      *out_type = leme_control_type_any();
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_FIRST:
    if (arg_count > 0 && arg_types != NULL &&
        arg_types[0].kind == LEME_CONTROL_TYPE_ARRAY &&
        arg_types[0].item_type != NULL) {
      *out_type = *arg_types[0].item_type;
      out_type->nullable = true;
    } else {
      *out_type = leme_control_type_any();
      out_type->nullable = true;
    }
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_COUNT:
    *out_type = leme_control_type_number();
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_SELECT:
  case LEME_CONTROL_OP_MAP:
    *out_type = (struct leme_control_type){
        .kind = LEME_CONTROL_TYPE_ARRAY,
        .provenance = LEME_CONTROL_PROVENANCE_SYNTHETIC,
    };
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_ADD:
  case LEME_CONTROL_OP_SUB:
  case LEME_CONTROL_OP_MUL:
  case LEME_CONTROL_OP_DIV:
    *out_type = leme_control_type_number();
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_AND:
  case LEME_CONTROL_OP_OR:
  case LEME_CONTROL_OP_NOT:
  case LEME_CONTROL_OP_EQ:
  case LEME_CONTROL_OP_NE:
  case LEME_CONTROL_OP_LT:
  case LEME_CONTROL_OP_LE:
  case LEME_CONTROL_OP_GT:
  case LEME_CONTROL_OP_GE:
    *out_type = leme_control_type_boolean();
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_IF:
    if (arg_count >= 3 && arg_types != NULL) {
      if (leme_control_type_equal(&arg_types[1], &arg_types[2]))
        *out_type = arg_types[1];
      else
        *out_type = leme_control_type_any();
    } else {
      *out_type = leme_control_type_any();
    }
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_LIST: {
    static const struct leme_control_type any_item = {
        .kind = LEME_CONTROL_TYPE_ANY,
    };
    if (arg_count > 0 && arg_types != NULL) {
      bool uniform_entity = (arg_types[0].kind == LEME_CONTROL_TYPE_ENTITY);
      if (uniform_entity) {
        for (size_t i = 1; i < arg_count; ++i) {
          if (!leme_control_type_equal(&arg_types[0], &arg_types[i])) {
            uniform_entity = false;
            break;
          }
        }
      }
      if (uniform_entity) {
        *out_type = *leme_control_type_collection_ref(arg_types[0].entity,
                                                      arg_types[0].provenance);
        return LEME_CONTROL_OK;
      }
    }
    *out_type = leme_control_type_array(&any_item);
    return LEME_CONTROL_OK;
  }
  default:
    *out_type = leme_control_type_any();
    return LEME_CONTROL_OK;
  }
}

bool leme_control_is_valid_action_target(
    const struct leme_control_operator *op, size_t arg_index,
    const struct leme_control_type *target_type) {
  if (op == NULL || target_type == NULL)
    return false;

  bool is_target_arg = false;
  enum leme_public_entity expected_entity = LEME_PUBLIC_VIEW;

  switch (op->opcode) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CLOSE:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_VIEW;
    }
    break;
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_SET_LAYOUT:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_TAG;
    }
    break;
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_OUTPUT;
    }
    break;
  case LEME_CONTROL_OP_SET_INPUT:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_INPUT;
    }
    break;
  case LEME_CONTROL_OP_MOVE_TO_TAG:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_VIEW;
    } else if (arg_index == 1) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_TAG;
    }
    break;
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
    if (arg_index == 0) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_VIEW;
    } else if (arg_index == 1) {
      is_target_arg = true;
      expected_entity = LEME_PUBLIC_OUTPUT;
    }
    break;
  default:
    break;
  }

  if (!is_target_arg)
    return true;

  if (target_type->kind == LEME_CONTROL_TYPE_ARRAY) {
    if (target_type->item_type == NULL)
      return false;
    return target_type->item_type->kind == LEME_CONTROL_TYPE_ENTITY &&
           target_type->item_type->entity == expected_entity &&
           target_type->item_type->provenance ==
               LEME_CONTROL_PROVENANCE_SNAPSHOT;
  }

  if (target_type->kind == LEME_CONTROL_TYPE_ENTITY) {
    return target_type->entity == expected_entity &&
           target_type->provenance == LEME_CONTROL_PROVENANCE_SNAPSHOT;
  }

  return false;
}
