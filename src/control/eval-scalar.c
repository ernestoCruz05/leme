#include "control/eval-internal.h"
#include "public/value-internal.h"

#include <math.h>
#include <string.h>

static enum leme_control_code
make_bool(struct evaluator *ev, bool b, const struct leme_public_value **out) {
  struct leme_public_value *res = NULL;
  enum leme_public_status status = leme_public_boolean(ev->builder, b, &res);
  if (status != LEME_PUBLIC_OK) {
    enum leme_control_code code =
        (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                     : LEME_CONTROL_OUT_OF_MEMORY);
    return eval_set_error(ev, "/expr", code, "boolean allocation failed");
  }
  *out = res;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
make_number(struct evaluator *ev, double n,
            const struct leme_public_value **out) {
  if (!isfinite(n) || fabs(n) > 9007199254740991.0)
    return eval_set_error(
        ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
        "arithmetic result not finite or exceeds safe integer");
  struct leme_public_value *res = NULL;
  enum leme_public_status status = leme_public_number(ev->builder, n, &res);
  if (status != LEME_PUBLIC_OK) {
    enum leme_control_code code =
        (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                     : LEME_CONTROL_OUT_OF_MEMORY);
    return eval_set_error(ev, "/expr", code, "number allocation failed");
  }
  *out = res;
  return LEME_CONTROL_OK;
}

bool eval_equal(struct evaluator *ev, const struct leme_public_value *left,
                const struct leme_public_value *right) {
  if (left == right)
    return true;
  if (left == NULL || right == NULL)
    return false;
  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK) {
    eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                   "evaluation work limit exceeded");
    return false;
  }
  if (leme_public_kind(left) != leme_public_kind(right))
    return false;
  switch (leme_public_kind(left)) {
  case LEME_PUBLIC_NULL:
    return true;
  case LEME_PUBLIC_BOOLEAN: {
    bool b1 = false, b2 = false;
    leme_public_as_bool(left, &b1);
    leme_public_as_bool(right, &b2);
    return b1 == b2;
  }
  case LEME_PUBLIC_NUMBER: {
    double n1 = 0.0, n2 = 0.0;
    leme_public_as_number(left, &n1);
    leme_public_as_number(right, &n2);
    return n1 == n2;
  }
  case LEME_PUBLIC_STRING: {
    struct leme_public_text t1 = {0}, t2 = {0};
    leme_public_as_text(left, &t1);
    leme_public_as_text(right, &t2);
    if (t1.length != t2.length)
      return false;
    if (leme_control_charge(&ev->meter, t1.length) != LEME_CONTROL_OK) {
      eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                     "evaluation work limit exceeded");
      return false;
    }
    return memcmp(t1.data, t2.data, t1.length) == 0;
  }
  case LEME_PUBLIC_ARRAY: {
    size_t len1 = leme_public_length(left);
    size_t len2 = leme_public_length(right);
    if (len1 != len2)
      return false;
    for (size_t i = 0; i < len1; ++i) {
      if (ev->error != NULL && ev->error->code != 0)
        return false;
      if (!eval_equal(ev, leme_public_at(left, i), leme_public_at(right, i)))
        return false;
      if (ev->error != NULL && ev->error->code != 0)
        return false;
    }
    return true;
  }
  case LEME_PUBLIC_OBJECT: {
    size_t count1 = leme_public_length(left);
    size_t count2 = leme_public_length(right);
    if (count1 != count2)
      return false;
    for (size_t i = 0; i < count1; ++i) {
      if (ev->error != NULL && ev->error->code != 0)
        return false;
      struct leme_public_text k1 = leme_public_key_at(left, i);
      const struct leme_public_value *v1 = leme_public_member_at(left, i);
      const struct leme_public_value *v2 = leme_public_get(right, k1);
      if (v2 == NULL || !eval_equal(ev, v1, v2))
        return false;
      if (ev->error != NULL && ev->error->code != 0)
        return false;
    }
    return true;
  }
  }
  return false;
}

const struct leme_public_value *
eval_ensure_owned(struct evaluator *ev, const struct leme_public_value *val,
                  enum leme_control_code *out_code) {
  if (val == NULL)
    return NULL;
  if (val->owner == ev->builder)
    return val;
  struct leme_public_value *cloned = NULL;
  enum leme_public_status status =
      leme_public_clone(ev->builder, val, &cloned);
  if (status != LEME_PUBLIC_OK) {
    enum leme_control_code code =
        (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                     : LEME_CONTROL_OUT_OF_MEMORY);
    if (out_code != NULL)
      *out_code = code;
    eval_set_error(ev, "/expr", code,
                   "failed to clone value into evaluation arena");
    return NULL;
  }
  return cloned;
}

enum leme_control_code
eval_scalar_call(struct evaluator *ev, const struct leme_control_node *node,
                 const struct leme_public_value **out) {
  const struct leme_control_operator *op = node->as.call.op;
  const uint32_t *args = node->as.call.arg_indices;
  size_t arg_count = node->as.call.arg_count;

  switch (op->opcode) {
  case LEME_CONTROL_OP_ADD: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    double n0 = 0.0, n1 = 0.0;
    if (leme_public_as_number(v0, &n0) != LEME_PUBLIC_OK ||
        leme_public_as_number(v1, &n1) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "add operands must be numbers");
    return make_number(ev, n0 + n1, out);
  }
  case LEME_CONTROL_OP_SUB: {
    if (arg_count == 1) {
      const struct leme_public_value *v0 = NULL;
      enum leme_control_code code = eval_node(ev, args[0], &v0);
      if (code != LEME_CONTROL_OK)
        return code;
      double n0 = 0.0;
      if (leme_public_as_number(v0, &n0) != LEME_PUBLIC_OK)
        return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                              "unary minus operand must be number");
      return make_number(ev, -n0, out);
    }
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    double n0 = 0.0, n1 = 0.0;
    if (leme_public_as_number(v0, &n0) != LEME_PUBLIC_OK ||
        leme_public_as_number(v1, &n1) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "sub operands must be numbers");
    return make_number(ev, n0 - n1, out);
  }
  case LEME_CONTROL_OP_MUL: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    double n0 = 0.0, n1 = 0.0;
    if (leme_public_as_number(v0, &n0) != LEME_PUBLIC_OK ||
        leme_public_as_number(v1, &n1) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "mul operands must be numbers");
    return make_number(ev, n0 * n1, out);
  }
  case LEME_CONTROL_OP_DIV: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    double n0 = 0.0, n1 = 0.0;
    if (leme_public_as_number(v0, &n0) != LEME_PUBLIC_OK ||
        leme_public_as_number(v1, &n1) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "div operands must be numbers");
    if (n1 == 0.0 || n1 == -0.0)
      return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                            "division by zero");
    return make_number(ev, n0 / n1, out);
  }
  case LEME_CONTROL_OP_AND: {
    for (size_t i = 0; i < arg_count; ++i) {
      const struct leme_public_value *v = NULL;
      enum leme_control_code code = eval_node(ev, args[i], &v);
      if (code != LEME_CONTROL_OK)
        return code;
      bool b = false;
      if (leme_public_as_bool(v, &b) != LEME_PUBLIC_OK)
        return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                              "and operand must be boolean");
      if (!b)
        return make_bool(ev, false, out);
    }
    return make_bool(ev, true, out);
  }
  case LEME_CONTROL_OP_OR: {
    for (size_t i = 0; i < arg_count; ++i) {
      const struct leme_public_value *v = NULL;
      enum leme_control_code code = eval_node(ev, args[i], &v);
      if (code != LEME_CONTROL_OK)
        return code;
      bool b = false;
      if (leme_public_as_bool(v, &b) != LEME_PUBLIC_OK)
        return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                              "or operand must be boolean");
      if (b)
        return make_bool(ev, true, out);
    }
    return make_bool(ev, false, out);
  }
  case LEME_CONTROL_OP_NOT: {
    const struct leme_public_value *v = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v);
    if (code != LEME_CONTROL_OK)
      return code;
    bool b = false;
    if (leme_public_as_bool(v, &b) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "not operand must be boolean");
    return make_bool(ev, !b, out);
  }
  case LEME_CONTROL_OP_IF: {
    const struct leme_public_value *cond_val = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &cond_val);
    if (code != LEME_CONTROL_OK)
      return code;
    bool cond = false;
    if (leme_public_as_bool(cond_val, &cond) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "if condition must be boolean");
    if (cond)
      return eval_node(ev, args[1], out);
    return eval_node(ev, args[2], out);
  }
  case LEME_CONTROL_OP_EQ: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    bool eq = eval_equal(ev, v0, v1);
    if (ev->error != NULL && ev->error->code != 0)
      return ev->error->code;
    return make_bool(ev, eq, out);
  }
  case LEME_CONTROL_OP_NE: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;
    bool eq = eval_equal(ev, v0, v1);
    if (ev->error != NULL && ev->error->code != 0)
      return ev->error->code;
    return make_bool(ev, !eq, out);
  }
  case LEME_CONTROL_OP_LT:
  case LEME_CONTROL_OP_LE:
  case LEME_CONTROL_OP_GT:
  case LEME_CONTROL_OP_GE: {
    const struct leme_public_value *v0 = NULL, *v1 = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v0);
    if (code != LEME_CONTROL_OK)
      return code;
    code = eval_node(ev, args[1], &v1);
    if (code != LEME_CONTROL_OK)
      return code;

    enum leme_public_kind k0 = leme_public_kind(v0);
    enum leme_public_kind k1 = leme_public_kind(v1);

    if (k0 == LEME_PUBLIC_NUMBER && k1 == LEME_PUBLIC_NUMBER) {
      double n0 = 0.0, n1 = 0.0;
      leme_public_as_number(v0, &n0);
      leme_public_as_number(v1, &n1);
      bool r = false;
      if (op->opcode == LEME_CONTROL_OP_LT)
        r = n0 < n1;
      else if (op->opcode == LEME_CONTROL_OP_LE)
        r = n0 <= n1;
      else if (op->opcode == LEME_CONTROL_OP_GT)
        r = n0 > n1;
      else
        r = n0 >= n1;
      return make_bool(ev, r, out);
    }
    if (k0 == LEME_PUBLIC_STRING && k1 == LEME_PUBLIC_STRING) {
      struct leme_public_text t0 = {0}, t1 = {0};
      leme_public_as_text(v0, &t0);
      leme_public_as_text(v1, &t1);
      size_t min_len = t0.length < t1.length ? t0.length : t1.length;
      if (leme_control_charge(&ev->meter, min_len + 1) != LEME_CONTROL_OK) {
        return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                              "evaluation work limit exceeded");
      }
      int cmp = memcmp(t0.data, t1.data, min_len);
      if (cmp == 0) {
        if (t0.length < t1.length)
          cmp = -1;
        else if (t0.length > t1.length)
          cmp = 1;
      }
      bool r = false;
      if (op->opcode == LEME_CONTROL_OP_LT)
        r = cmp < 0;
      else if (op->opcode == LEME_CONTROL_OP_LE)
        r = cmp <= 0;
      else if (op->opcode == LEME_CONTROL_OP_GT)
        r = cmp > 0;
      else
        r = cmp >= 0;
      return make_bool(ev, r, out);
    }
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "ordering requires numbers or strings");
  }
  case LEME_CONTROL_OP_LIST: {
    if (leme_control_charge(&ev->meter, arg_count > 0 ? arg_count : 1) !=
        LEME_CONTROL_OK) {
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "evaluation work limit exceeded");
    }
    struct leme_public_value *arr = NULL;
    enum leme_public_status status =
        leme_public_array(ev->builder, arg_count, &arr);
    if (status != LEME_PUBLIC_OK) {
      enum leme_control_code code =
          (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                       : LEME_CONTROL_OUT_OF_MEMORY);
      return eval_set_error(ev, "/expr", code, "array allocation failed");
    }
    for (size_t i = 0; i < arg_count; ++i) {
      const struct leme_public_value *child_v = NULL;
      enum leme_control_code code = eval_node(ev, args[i], &child_v);
      if (code != LEME_CONTROL_OK)
        return code;
      const struct leme_public_value *owned =
          eval_ensure_owned(ev, child_v, &code);
      if (owned == NULL)
        return code;
      leme_public_array_set(ev->builder, arr, i, owned);
    }
    *out = arr;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_OBJECT: {
    if (arg_count % 2 != 0)
      return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                            "object requires even number of arguments");
    size_t pairs = arg_count / 2;
    if (leme_control_charge(&ev->meter, pairs > 0 ? pairs : 1) !=
        LEME_CONTROL_OK) {
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "evaluation work limit exceeded");
    }
    struct leme_public_value *obj = NULL;
    enum leme_public_status status =
        leme_public_object(ev->builder, pairs, &obj);
    if (status != LEME_PUBLIC_OK) {
      enum leme_control_code code =
          (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                       : LEME_CONTROL_OUT_OF_MEMORY);
      return eval_set_error(ev, "/expr", code, "object allocation failed");
    }
    for (size_t p = 0; p < pairs; ++p) {
      const struct leme_public_value *kv = NULL;
      enum leme_control_code code = eval_node(ev, args[2 * p], &kv);
      if (code != LEME_CONTROL_OK)
        return code;
      struct leme_public_text ktext = {0};
      if (leme_public_as_text(kv, &ktext) != LEME_PUBLIC_OK)
        return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                              "object key must be string");
      if (leme_public_get(obj, ktext) != NULL)
        return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                              "duplicate object key");
      const struct leme_public_value *vv = NULL;
      code = eval_node(ev, args[2 * p + 1], &vv);
      if (code != LEME_CONTROL_OK)
        return code;
      const struct leme_public_value *owned =
          eval_ensure_owned(ev, vv, &code);
      if (owned == NULL)
        return code;
      leme_public_object_set(ev->builder, obj, ktext, owned);
    }
    *out = obj;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_CONTAINS: {
    const struct leme_public_value *v_arr = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v_arr);
    if (code != LEME_CONTROL_OK)
      return code;
    if (leme_public_kind(v_arr) != LEME_PUBLIC_ARRAY)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "contains requires array");
    const struct leme_public_value *v_tgt = NULL;
    code = eval_node(ev, args[1], &v_tgt);
    if (code != LEME_CONTROL_OK)
      return code;
    size_t len = leme_public_length(v_arr);
    bool found = false;
    for (size_t i = 0; i < len; ++i) {
      if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
        return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                              "evaluation work limit exceeded");
      const struct leme_public_value *item = leme_public_at(v_arr, i);
      if (eval_equal(ev, item, v_tgt)) {
        found = true;
        break;
      }
      if (ev->error != NULL && ev->error->code != 0)
        return ev->error->code;
    }
    if (ev->error != NULL && ev->error->code != 0)
      return ev->error->code;
    return make_bool(ev, found, out);
  }
  case LEME_CONTROL_OP_GET: {
    const struct leme_public_value *v_cont = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &v_cont);
    if (code != LEME_CONTROL_OK)
      return code;
    const struct leme_public_value *v_key = NULL;
    code = eval_node(ev, args[1], &v_key);
    if (code != LEME_CONTROL_OK)
      return code;
    struct leme_public_text ktext = {0};
    if (leme_public_as_text(v_key, &ktext) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "get key must be string");
    if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "evaluation work limit exceeded");
    if (v_cont == NULL || leme_public_kind(v_cont) != LEME_PUBLIC_OBJECT)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "get requires object");
    const struct leme_public_value *member = leme_public_get(v_cont, ktext);
    if (member == NULL)
      return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                            "member not found");
    *out = member;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_COUNT: {
    const struct leme_public_value *arr = NULL;
    enum leme_control_code code = eval_node(ev, args[0], &arr);
    if (code != LEME_CONTROL_OK)
      return code;
    if (arr == NULL || leme_public_kind(arr) != LEME_PUBLIC_ARRAY)
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "count requires array");
    if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "evaluation work limit exceeded");
    return make_number(ev, (double)leme_public_length(arr), out);
  }
  case LEME_CONTROL_OP_ITEM: {
    if (ev->current_item == NULL)
      return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                            "item outside scope");
    *out = ev->current_item;
    return LEME_CONTROL_OK;
  }
  default:
    return eval_set_error(ev, "/expr", LEME_CONTROL_UNSUPPORTED,
                          "unsupported operator");
  }
}
