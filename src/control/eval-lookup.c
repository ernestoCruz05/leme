#include "control/eval-internal.h"
#include "control/registry.h"
#include "public/model.h"
#include "public/schema.h"
#include "public/value.h"

#include <math.h>
#include <string.h>

static enum leme_control_code eval_root(struct evaluator *ev,
                                        enum leme_control_opcode opcode,
                                        const struct leme_public_value **out) {
  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");
  enum leme_public_root root;
  switch (opcode) {
  case LEME_CONTROL_OP_VIEWS:
    root = LEME_PUBLIC_VIEWS;
    break;
  case LEME_CONTROL_OP_TAGS:
    root = LEME_PUBLIC_TAGS;
    break;
  case LEME_CONTROL_OP_OUTPUTS:
    root = LEME_PUBLIC_OUTPUTS;
    break;
  case LEME_CONTROL_OP_INPUTS:
    root = LEME_PUBLIC_INPUTS;
    break;
  case LEME_CONTROL_OP_SESSION:
    root = LEME_PUBLIC_SESSION;
    break;
  case LEME_CONTROL_OP_CONFIG:
    root = LEME_PUBLIC_CONFIG;
    break;
  case LEME_CONTROL_OP_RUNTIME:
    root = LEME_PUBLIC_RUNTIME;
    break;
  case LEME_CONTROL_OP_STATUS:
    root = LEME_PUBLIC_STATUS;
    break;
  default:
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_REQUEST,
                          "invalid root opcode");
  }
  const struct leme_public_value *val =
      leme_public_snapshot_root(ev->snapshot, root);
  if (val == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "root unavailable in snapshot");
  size_t units = 1;
  if (leme_public_kind(val) == LEME_PUBLIC_ARRAY)
    units = leme_public_length(val);
  if (leme_control_charge(&ev->meter, units) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");
  *out = val;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_view(struct evaluator *ev,
                                        const struct leme_control_node *node,
                                        const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  if (arg0 == NULL || leme_public_kind(arg0) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "view id must be a string");
  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");
  struct leme_public_text id = {0};
  leme_public_as_text(arg0, &id);
  enum leme_public_status st =
      leme_public_snapshot_find(ev->snapshot, LEME_PUBLIC_VIEW, id, out);
  if (st == LEME_PUBLIC_NOT_FOUND)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "view not found");
  if (st != LEME_PUBLIC_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "view lookup failed");
  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_input(struct evaluator *ev,
                                         const struct leme_control_node *node,
                                         const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  if (arg0 == NULL || leme_public_kind(arg0) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "input id must be a string");
  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");
  struct leme_public_text id = {0};
  leme_public_as_text(arg0, &id);
  enum leme_public_status st =
      leme_public_snapshot_find(ev->snapshot, LEME_PUBLIC_INPUT, id, out);
  if (st == LEME_PUBLIC_NOT_FOUND)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "input device not found");
  if (st != LEME_PUBLIC_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "input lookup failed");
  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");
  return LEME_CONTROL_OK;
}

static enum leme_control_code
eval_output(struct evaluator *ev, const struct leme_control_node *node,
            const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  if (arg0 == NULL || leme_public_kind(arg0) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "output name must be a string");
  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");
  struct leme_public_text target_name = {0};
  leme_public_as_text(arg0, &target_name);
  const struct leme_public_value *outputs =
      leme_public_snapshot_root(ev->snapshot, LEME_PUBLIC_OUTPUTS);
  if (outputs == NULL || leme_public_kind(outputs) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "outputs root unavailable");
  size_t n = leme_public_length(outputs);
  const struct leme_public_value *matched = NULL;
  size_t match_count = 0;
  for (size_t i = 0; i < n; ++i) {
    const struct leme_public_value *rec = leme_public_at(outputs, i);
    if (rec == NULL || leme_public_kind(rec) != LEME_PUBLIC_OBJECT)
      continue;
    const struct leme_public_value *name_val =
        leme_public_get(rec, LEME_PUBLIC_TEXT("name"));
    if (name_val == NULL || leme_public_kind(name_val) != LEME_PUBLIC_STRING)
      continue;
    struct leme_public_text name = {0};
    leme_public_as_text(name_val, &name);
    if (name.length == target_name.length &&
        memcmp(name.data, target_name.data, target_name.length) == 0) {
      match_count++;
      matched = rec;
    }
  }
  if (leme_control_charge(&ev->meter, n > 0 ? n : 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");
  if (match_count == 0)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "output not found");
  if (match_count > 1)
    return eval_set_error(ev, "/expr", LEME_CONTROL_CARDINALITY,
                          "output connector name ambiguous");
  *out = matched;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_tag(struct evaluator *ev,
                                       const struct leme_control_node *node,
                                       const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  const struct leme_public_value *arg1 = NULL;
  code = eval_node(ev, node->as.call.arg_indices[1], &arg1);
  if (code != LEME_CONTROL_OK)
    return code;

  if (arg1 == NULL || leme_public_kind(arg1) != LEME_PUBLIC_NUMBER)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "tag slot must be a number");
  double slot_num = 0.0;
  leme_public_as_number(arg1, &slot_num);
  if (!isfinite(slot_num) || floor(slot_num) != slot_num || slot_num < 1.0 ||
      slot_num > 64.0)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "tag slot out of range [1, 64]");
  uint16_t slot = (uint16_t)slot_num;

  struct leme_public_text output_id = {0};
  if (arg0 != NULL && leme_public_kind(arg0) == LEME_PUBLIC_STRING) {
    if (ev->snapshot == NULL)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "snapshot not available");
    struct leme_public_text conn_name = {0};
    leme_public_as_text(arg0, &conn_name);
    const struct leme_public_value *outputs =
        leme_public_snapshot_root(ev->snapshot, LEME_PUBLIC_OUTPUTS);
    if (outputs == NULL || leme_public_kind(outputs) != LEME_PUBLIC_ARRAY)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "outputs root unavailable");
    size_t n = leme_public_length(outputs);
    const struct leme_public_value *matched_out = NULL;
    size_t match_count = 0;
    for (size_t i = 0; i < n; ++i) {
      const struct leme_public_value *rec = leme_public_at(outputs, i);
      if (rec == NULL || leme_public_kind(rec) != LEME_PUBLIC_OBJECT)
        continue;
      const struct leme_public_value *name_val =
          leme_public_get(rec, LEME_PUBLIC_TEXT("name"));
      if (name_val == NULL || leme_public_kind(name_val) != LEME_PUBLIC_STRING)
        continue;
      struct leme_public_text name = {0};
      leme_public_as_text(name_val, &name);
      if (name.length == conn_name.length &&
          memcmp(name.data, conn_name.data, conn_name.length) == 0) {
        match_count++;
        matched_out = rec;
      }
    }
    if (leme_control_charge(&ev->meter, n > 0 ? n : 1) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "work limit exceeded");
    if (match_count == 0)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "output not found");
    if (match_count > 1)
      return eval_set_error(ev, "/expr", LEME_CONTROL_CARDINALITY,
                            "output connector name ambiguous");
    const struct leme_public_value *id_val =
        leme_public_get(matched_out, LEME_PUBLIC_TEXT("id"));
    if (id_val == NULL || leme_public_kind(id_val) != LEME_PUBLIC_STRING)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "output id not found");
    leme_public_as_text(id_val, &output_id);
  } else if (arg0 != NULL && leme_public_kind(arg0) == LEME_PUBLIC_OBJECT) {
    const struct leme_public_value *id_val =
        leme_public_get(arg0, LEME_PUBLIC_TEXT("id"));
    if (id_val == NULL || leme_public_kind(id_val) != LEME_PUBLIC_STRING)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "tag target object must have string id");
    leme_public_as_text(id_val, &output_id);
  } else {
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "tag target must be output or string");
  }

  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");

  const struct leme_public_value *tags =
      leme_public_snapshot_root(ev->snapshot, LEME_PUBLIC_TAGS);
  if (tags == NULL || leme_public_kind(tags) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "tags root unavailable");

  size_t tag_count = leme_public_length(tags);
  const struct leme_public_value *matched_tag = NULL;
  for (size_t i = 0; i < tag_count; ++i) {
    const struct leme_public_value *tag_rec = leme_public_at(tags, i);
    if (tag_rec == NULL || leme_public_kind(tag_rec) != LEME_PUBLIC_OBJECT)
      continue;
    const struct leme_public_value *num_val =
        leme_public_get(tag_rec, LEME_PUBLIC_TEXT("number"));
    if (num_val == NULL || leme_public_kind(num_val) != LEME_PUBLIC_NUMBER)
      continue;
    double tnum = 0.0;
    leme_public_as_number(num_val, &tnum);
    if ((uint16_t)tnum != slot)
      continue;
    const struct leme_public_value *out_ref =
        leme_public_get(tag_rec, LEME_PUBLIC_TEXT("output"));
    if (out_ref == NULL || leme_public_kind(out_ref) != LEME_PUBLIC_OBJECT)
      continue;
    const struct leme_public_value *out_ref_id =
        leme_public_get(out_ref, LEME_PUBLIC_TEXT("id"));
    if (out_ref_id == NULL ||
        leme_public_kind(out_ref_id) != LEME_PUBLIC_STRING)
      continue;
    struct leme_public_text ref_id_text = {0};
    leme_public_as_text(out_ref_id, &ref_id_text);
    if (ref_id_text.length == output_id.length &&
        memcmp(ref_id_text.data, output_id.data, output_id.length) == 0) {
      matched_tag = tag_rec;
      break;
    }
  }

  if (leme_control_charge(&ev->meter, tag_count > 0 ? tag_count : 1) !=
      LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");

  if (matched_tag == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND, "tag not found");

  *out = matched_tag;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_by_id(struct evaluator *ev,
                                         const struct leme_control_node *node,
                                         const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  const struct leme_public_value *arg1 = NULL;
  code = eval_node(ev, node->as.call.arg_indices[1], &arg1);
  if (code != LEME_CONTROL_OK)
    return code;

  if (arg0 == NULL || leme_public_kind(arg0) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "by-id kind must be a string");
  if (arg1 == NULL || leme_public_kind(arg1) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "by-id id must be a string");

  struct leme_public_text kind_text = {0};
  leme_public_as_text(arg0, &kind_text);
  enum leme_public_entity entity_kind;
  if (!eval_entity_kind(kind_text, &entity_kind))
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "invalid by-id kind");

  if (ev->snapshot == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "snapshot not available");

  struct leme_public_text id_text = {0};
  leme_public_as_text(arg1, &id_text);
  enum leme_public_status st =
      leme_public_snapshot_find(ev->snapshot, entity_kind, id_text, out);
  if (st == LEME_PUBLIC_NOT_FOUND)
    return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                          "entity not found");
  if (st != LEME_PUBLIC_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "by-id lookup failed");

  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");

  return LEME_CONTROL_OK;
}

static enum leme_control_code
eval_describe(struct evaluator *ev, const struct leme_control_node *node,
              const struct leme_public_value **out) {
  const struct leme_public_value *arg0 = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &arg0);
  if (code != LEME_CONTROL_OK)
    return code;
  const struct leme_public_value *arg1 = NULL;
  code = eval_node(ev, node->as.call.arg_indices[1], &arg1);
  if (code != LEME_CONTROL_OK)
    return code;

  if (arg0 == NULL || leme_public_kind(arg0) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "describe category must be a string");
  if (arg1 == NULL || leme_public_kind(arg1) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "describe target must be a string");

  struct leme_public_text cat = {0};
  leme_public_as_text(arg0, &cat);
  struct leme_public_text target = {0};
  leme_public_as_text(arg1, &target);

  struct leme_public_value *res = NULL;
  if (cat.length == 8 && memcmp(cat.data, "operator", 8) == 0) {
    const struct leme_public_operation *op =
        leme_control_operation_find(target);
    if (op == NULL)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "operator not found");
    enum leme_public_status st =
        leme_public_operation_value(ev->builder, op, &res);
    if (st != LEME_PUBLIC_OK) {
      enum leme_control_code c =
          (st == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                   : LEME_CONTROL_OUT_OF_MEMORY);
      return eval_set_error(ev, "/expr", c,
                            "operation descriptor build failed");
    }
  } else if (cat.length == 4 && memcmp(cat.data, "root", 4) == 0) {
    enum leme_public_status st =
        leme_public_root_describe(ev->builder, target, &res);
    if (st == LEME_PUBLIC_NOT_FOUND)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "root not found");
    if (st != LEME_PUBLIC_OK) {
      enum leme_control_code c =
          (st == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                   : LEME_CONTROL_OUT_OF_MEMORY);
      return eval_set_error(ev, "/expr", c, "root descriptor build failed");
    }
  } else if (cat.length == 4 && memcmp(cat.data, "type", 4) == 0) {
    enum leme_public_status st =
        leme_public_type_describe(ev->builder, target, NULL, &res);
    if (st == LEME_PUBLIC_NOT_FOUND)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "type not found");
    if (st != LEME_PUBLIC_OK) {
      enum leme_control_code c =
          (st == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                   : LEME_CONTROL_OUT_OF_MEMORY);
      return eval_set_error(ev, "/expr", c, "type descriptor build failed");
    }
  } else {
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "invalid describe category");
  }

  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded");

  *out = res;
  return LEME_CONTROL_OK;
}

enum leme_control_code eval_lookup_call(struct evaluator *ev,
                                        const struct leme_control_node *node,
                                        const struct leme_public_value **out) {
  if (ev == NULL || node == NULL || out == NULL || node->as.call.op == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "invalid eval call");

  enum leme_control_opcode op = node->as.call.op->opcode;
  if (op <= LEME_CONTROL_OP_STATUS)
    return eval_root(ev, op, out);
  switch (op) {
  case LEME_CONTROL_OP_VIEW:
    return eval_view(ev, node, out);
  case LEME_CONTROL_OP_INPUT:
    return eval_input(ev, node, out);
  case LEME_CONTROL_OP_OUTPUT:
    return eval_output(ev, node, out);
  case LEME_CONTROL_OP_TAG:
    return eval_tag(ev, node, out);
  case LEME_CONTROL_OP_BY_ID:
    return eval_by_id(ev, node, out);
  case LEME_CONTROL_OP_DESCRIBE:
    return eval_describe(ev, node, out);
  default:
    return eval_set_error(ev, "/expr", LEME_CONTROL_UNSUPPORTED,
                          "unsupported lookup operator");
  }
}
