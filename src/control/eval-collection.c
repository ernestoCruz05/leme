#include "control/eval-internal.h"
#include "control/memory.h"
#include "public/value-internal.h"

#include <math.h>
#include <string.h>

struct sort_entry {
  const struct leme_public_value *item;
  const struct leme_public_value *key;
  size_t original_index;
};

static enum leme_control_code compare_entries(struct evaluator *ev,
                                              const struct sort_entry *a,
                                              const struct sort_entry *b,
                                              bool is_desc, int *out_cmp) {
  bool a_null =
      (a->key == NULL || leme_public_kind(a->key) == LEME_PUBLIC_NULL);
  bool b_null =
      (b->key == NULL || leme_public_kind(b->key) == LEME_PUBLIC_NULL);
  if (a_null && b_null) {
    *out_cmp = (a->original_index < b->original_index) ? -1 : 1;
    return LEME_CONTROL_OK;
  }
  if (a_null) {
    *out_cmp = 1;
    return LEME_CONTROL_OK;
  }
  if (b_null) {
    *out_cmp = -1;
    return LEME_CONTROL_OK;
  }
  int raw_cmp = 0;
  enum leme_public_kind kind = leme_public_kind(a->key);
  if (kind == LEME_PUBLIC_NUMBER) {
    if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "work limit exceeded in sort");
    double na = 0.0, nb = 0.0;
    leme_public_as_number(a->key, &na);
    leme_public_as_number(b->key, &nb);
    if (na < nb)
      raw_cmp = -1;
    else if (na > nb)
      raw_cmp = 1;
    else
      raw_cmp = 0;
  } else {
    struct leme_public_text ta = {0}, tb = {0};
    leme_public_as_text(a->key, &ta);
    leme_public_as_text(b->key, &tb);
    size_t min_len = ta.length < tb.length ? ta.length : tb.length;
    if (leme_control_charge(&ev->meter, 1 + min_len) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "work limit exceeded in sort");
    int c = memcmp(ta.data, tb.data, min_len);
    if (c != 0)
      raw_cmp = (c < 0) ? -1 : 1;
    else if (ta.length != tb.length)
      raw_cmp = (ta.length < tb.length) ? -1 : 1;
    else
      raw_cmp = 0;
  }
  int directed_cmp = is_desc ? -raw_cmp : raw_cmp;
  if (directed_cmp == 0)
    directed_cmp = (a->original_index < b->original_index) ? -1 : 1;
  *out_cmp = directed_cmp;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_where(struct evaluator *ev,
                                         const struct leme_control_node *node,
                                         const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "where requires array");

  size_t n = leme_public_length(col);
  if (n == 0) {
    struct leme_public_value *empty = NULL;
    if (leme_public_array(ev->builder, 0, &empty) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array allocation failed");
    *out = empty;
    return LEME_CONTROL_OK;
  }

  bool *matched = leme_control_alloc(ev->account, n * sizeof(bool));
  if (matched == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "allocation failed");

  size_t match_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK) {
      leme_control_free(matched);
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "work limit exceeded in where");
    }
    const struct leme_public_value *item = leme_public_at(col, i);
    const struct leme_public_value *prev_item = ev->current_item;
    ev->current_item = item;
    const struct leme_public_value *pred_val = NULL;
    code = eval_node(ev, node->as.call.arg_indices[1], &pred_val);
    ev->current_item = prev_item;
    if (code != LEME_CONTROL_OK) {
      leme_control_free(matched);
      return code;
    }
    if (pred_val == NULL || leme_public_kind(pred_val) != LEME_PUBLIC_BOOLEAN) {
      leme_control_free(matched);
      return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                            "where predicate must evaluate to boolean");
    }
    bool b = false;
    leme_public_as_bool(pred_val, &b);
    matched[i] = b;
    if (b)
      match_count++;
  }

  struct leme_public_value *res = NULL;
  if (leme_public_array(ev->builder, match_count, &res) != LEME_PUBLIC_OK) {
    leme_control_free(matched);
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "array allocation failed");
  }

  size_t out_idx = 0;
  for (size_t i = 0; i < n; ++i) {
    if (matched[i]) {
      const struct leme_public_value *item = leme_public_at(col, i);
      const struct leme_public_value *owned =
          eval_ensure_owned(ev, item, &code);
      if (owned == NULL) {
        leme_control_free(matched);
        return code;
      }
      if (leme_public_array_set(ev->builder, res, out_idx++, owned) !=
          LEME_PUBLIC_OK) {
        leme_control_free(matched);
        return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                              "array set failed");
      }
    }
  }

  leme_control_free(matched);
  *out = res;
  return LEME_CONTROL_OK;
}

struct select_key {
  struct leme_public_text text;
  char *buf;
};

static void free_select_keys(struct select_key *keys, size_t count) {
  if (keys == NULL)
    return;
  for (size_t k = 0; k < count; ++k)
    leme_control_free(keys[k].buf);
  leme_control_free(keys);
}

static enum leme_control_code
eval_select(struct evaluator *ev, const struct leme_control_node *node,
            const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "select requires array");

  size_t n = leme_public_length(col);
  size_t field_count = node->as.call.arg_count - 1;

  if (n == 0) {
    struct leme_public_value *empty = NULL;
    if (leme_public_array(ev->builder, 0, &empty) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array allocation failed");
    *out = empty;
    return LEME_CONTROL_OK;
  }

  struct select_key *keys =
      leme_control_alloc(ev->account, field_count * sizeof(struct select_key));
  if (keys == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "allocation failed");
  memset(keys, 0, field_count * sizeof(struct select_key));

  for (size_t j = 0; j < field_count; ++j) {
    const struct leme_control_node *fn = leme_control_program_node(
        ev->program, node->as.call.arg_indices[j + 1]);
    if (fn == NULL || fn->kind != LEME_CONTROL_NODE_FIELD) {
      free_select_keys(keys, field_count);
      return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                            "invalid field node in select");
    }
    if (fn->as.field.count == 1) {
      keys[j].text = fn->as.field.components[0];
      keys[j].buf = NULL;
    } else {
      size_t tot = fn->as.field.count - 1;
      for (size_t c = 0; c < fn->as.field.count; ++c)
        tot += fn->as.field.components[c].length;
      char *buf = leme_control_alloc(ev->account, tot + 1);
      if (buf == NULL) {
        free_select_keys(keys, field_count);
        return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                              "allocation failed");
      }
      size_t pos = 0;
      for (size_t c = 0; c < fn->as.field.count; ++c) {
        if (c > 0)
          buf[pos++] = '.';
        memcpy(buf + pos, fn->as.field.components[c].data,
               fn->as.field.components[c].length);
        pos += fn->as.field.components[c].length;
      }
      buf[pos] = '\0';
      keys[j].text = (struct leme_public_text){.data = buf, .length = tot};
      keys[j].buf = buf;
    }
  }

  struct leme_public_value *res = NULL;
  if (leme_public_array(ev->builder, n, &res) != LEME_PUBLIC_OK) {
    free_select_keys(keys, field_count);
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "array allocation failed");
  }

  for (size_t i = 0; i < n; ++i) {
    const struct leme_public_value *item = leme_public_at(col, i);
    struct leme_public_value *obj = NULL;
    if (leme_public_object(ev->builder, field_count, &obj) != LEME_PUBLIC_OK) {
      free_select_keys(keys, field_count);
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "object allocation failed");
    }

    const struct leme_public_value *prev_item = ev->current_item;
    ev->current_item = item;
    for (size_t j = 0; j < field_count; ++j) {
      if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK) {
        ev->current_item = prev_item;
        free_select_keys(keys, field_count);
        return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                              "work limit exceeded in select");
      }
      const struct leme_public_value *fval = NULL;
      code = eval_node(ev, node->as.call.arg_indices[j + 1], &fval);
      if (code != LEME_CONTROL_OK) {
        ev->current_item = prev_item;
        free_select_keys(keys, field_count);
        return code;
      }
      const struct leme_public_value *owned =
          eval_ensure_owned(ev, fval, &code);
      if (owned == NULL) {
        ev->current_item = prev_item;
        free_select_keys(keys, field_count);
        return code;
      }
      if (leme_public_object_set(ev->builder, obj, keys[j].text, owned) !=
          LEME_PUBLIC_OK) {
        ev->current_item = prev_item;
        free_select_keys(keys, field_count);
        return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                              "object set failed");
      }
    }
    ev->current_item = prev_item;
    if (leme_public_array_set(ev->builder, res, i, obj) != LEME_PUBLIC_OK) {
      free_select_keys(keys, field_count);
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array set failed");
    }
  }

  free_select_keys(keys, field_count);
  *out = res;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_map(struct evaluator *ev,
                                       const struct leme_control_node *node,
                                       const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "map requires array");

  size_t n = leme_public_length(col);
  if (n == 0) {
    struct leme_public_value *empty = NULL;
    if (leme_public_array(ev->builder, 0, &empty) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array allocation failed");
    *out = empty;
    return LEME_CONTROL_OK;
  }

  struct leme_public_value *res = NULL;
  if (leme_public_array(ev->builder, n, &res) != LEME_PUBLIC_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "array allocation failed");

  for (size_t i = 0; i < n; ++i) {
    if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "work limit exceeded in map");
    const struct leme_public_value *item = leme_public_at(col, i);
    const struct leme_public_value *prev_item = ev->current_item;
    ev->current_item = item;
    const struct leme_public_value *val = NULL;
    code = eval_node(ev, node->as.call.arg_indices[1], &val);
    ev->current_item = prev_item;
    if (code != LEME_CONTROL_OK)
      return code;
    const struct leme_public_value *owned = eval_ensure_owned(ev, val, &code);
    if (owned == NULL)
      return code;
    if (leme_public_array_set(ev->builder, res, i, owned) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array set failed");
  }

  *out = res;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
eval_sort_by(struct evaluator *ev, const struct leme_control_node *node,
             const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "sort-by requires array");

  const struct leme_public_value *dir_val = NULL;
  code = eval_node(ev, node->as.call.arg_indices[2], &dir_val);
  if (code != LEME_CONTROL_OK)
    return code;
  if (dir_val == NULL || leme_public_kind(dir_val) != LEME_PUBLIC_STRING)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "sort-by direction must be string");

  struct leme_public_text dir_text = {0};
  leme_public_as_text(dir_val, &dir_text);
  bool is_desc = false;
  if (dir_text.length == 3 && memcmp(dir_text.data, "asc", 3) == 0) {
    is_desc = false;
  } else if (dir_text.length == 4 && memcmp(dir_text.data, "desc", 4) == 0) {
    is_desc = true;
  } else {
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "sort-by direction must be 'asc' or 'desc'");
  }

  size_t n = leme_public_length(col);
  if (n == 0) {
    struct leme_public_value *empty = NULL;
    if (leme_public_array(ev->builder, 0, &empty) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array allocation failed");
    *out = empty;
    return LEME_CONTROL_OK;
  }

  struct sort_entry *entries =
      leme_control_alloc(ev->account, n * sizeof(struct sort_entry));
  if (entries == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "allocation failed");

  for (size_t i = 0; i < n; ++i) {
    entries[i].item = leme_public_at(col, i);
    entries[i].original_index = i;
    const struct leme_public_value *prev_item = ev->current_item;
    ev->current_item = entries[i].item;
    code = eval_node(ev, node->as.call.arg_indices[1], &entries[i].key);
    ev->current_item = prev_item;
    if (code != LEME_CONTROL_OK) {
      leme_control_free(entries);
      return code;
    }
  }

  enum leme_public_kind expected_kind = LEME_PUBLIC_NULL;
  for (size_t i = 0; i < n; ++i) {
    if (entries[i].key != NULL) {
      enum leme_public_kind k = leme_public_kind(entries[i].key);
      if (k != LEME_PUBLIC_NULL) {
        if (k != LEME_PUBLIC_NUMBER && k != LEME_PUBLIC_STRING) {
          leme_control_free(entries);
          return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                                "sort keys must be number or string");
        }
        if (expected_kind == LEME_PUBLIC_NULL) {
          expected_kind = k;
        } else if (expected_kind != k) {
          leme_control_free(entries);
          return eval_set_error(
              ev, "/expr", LEME_CONTROL_TYPE_ERROR,
              "sort keys must be homogeneous numbers or strings");
        }
      }
    }
  }

  if (n > 1) {
    struct sort_entry *aux =
        leme_control_alloc(ev->account, n * sizeof(struct sort_entry));
    if (aux == NULL) {
      leme_control_free(entries);
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "allocation failed");
    }

    for (size_t width = 1; width < n; width *= 2) {
      for (size_t i = 0; i < n; i += 2 * width) {
        size_t left = i;
        size_t mid = (i + width < n) ? (i + width) : n;
        size_t right = (i + 2 * width < n) ? (i + 2 * width) : n;
        size_t p1 = left;
        size_t p2 = mid;
        size_t dest = left;
        while (p1 < mid && p2 < right) {
          int cmp = 0;
          code = compare_entries(ev, &entries[p1], &entries[p2], is_desc, &cmp);
          if (code != LEME_CONTROL_OK) {
            leme_control_free(aux);
            leme_control_free(entries);
            return code;
          }
          if (cmp <= 0)
            aux[dest++] = entries[p1++];
          else
            aux[dest++] = entries[p2++];
        }
        while (p1 < mid)
          aux[dest++] = entries[p1++];
        while (p2 < right)
          aux[dest++] = entries[p2++];
        memcpy(&entries[left], &aux[left],
               (right - left) * sizeof(struct sort_entry));
      }
    }
    leme_control_free(aux);
  }

  struct leme_public_value *res = NULL;
  if (leme_public_array(ev->builder, n, &res) != LEME_PUBLIC_OK) {
    leme_control_free(entries);
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "array allocation failed");
  }

  for (size_t i = 0; i < n; ++i) {
    const struct leme_public_value *owned =
        eval_ensure_owned(ev, entries[i].item, &code);
    if (owned == NULL) {
      leme_control_free(entries);
      return code;
    }
    if (leme_public_array_set(ev->builder, res, i, owned) != LEME_PUBLIC_OK) {
      leme_control_free(entries);
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array set failed");
    }
  }

  leme_control_free(entries);
  *out = res;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_limit(struct evaluator *ev,
                                         const struct leme_control_node *node,
                                         const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "limit requires array");

  const struct leme_public_value *cnt_val = NULL;
  code = eval_node(ev, node->as.call.arg_indices[1], &cnt_val);
  if (code != LEME_CONTROL_OK)
    return code;
  if (cnt_val == NULL || leme_public_kind(cnt_val) != LEME_PUBLIC_NUMBER)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "limit count must be number");

  double cnt = 0.0;
  leme_public_as_number(cnt_val, &cnt);
  if (!isfinite(cnt) || cnt < 0.0 || floor(cnt) != cnt)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "limit count must be a non-negative integer");

  size_t orig_len = leme_public_length(col);
  size_t n = cnt > (double)orig_len ? orig_len : (size_t)cnt;

  if (n > 0 && leme_control_charge(&ev->meter, n) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded in limit");

  struct leme_public_value *res = NULL;
  if (leme_public_array(ev->builder, n, &res) != LEME_PUBLIC_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                          "array allocation failed");

  for (size_t i = 0; i < n; ++i) {
    const struct leme_public_value *item = leme_public_at(col, i);
    const struct leme_public_value *owned = eval_ensure_owned(ev, item, &code);
    if (owned == NULL)
      return code;
    if (leme_public_array_set(ev->builder, res, i, owned) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "array set failed");
  }

  *out = res;
  return LEME_CONTROL_OK;
}

static enum leme_control_code eval_first(struct evaluator *ev,
                                         const struct leme_control_node *node,
                                         const struct leme_public_value **out) {
  const struct leme_public_value *col = NULL;
  enum leme_control_code code =
      eval_node(ev, node->as.call.arg_indices[0], &col);
  if (code != LEME_CONTROL_OK)
    return code;
  if (col == NULL || leme_public_kind(col) != LEME_PUBLIC_ARRAY)
    return eval_set_error(ev, "/expr", LEME_CONTROL_TYPE_ERROR,
                          "first requires array");

  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "work limit exceeded in first");

  size_t len = leme_public_length(col);
  if (len == 0) {
    struct leme_public_value *nv = NULL;
    if (leme_public_null(ev->builder, &nv) != LEME_PUBLIC_OK)
      return eval_set_error(ev, "/expr", LEME_CONTROL_OUT_OF_MEMORY,
                            "null allocation failed");
    *out = nv;
    return LEME_CONTROL_OK;
  }

  const struct leme_public_value *item = leme_public_at(col, 0);
  const struct leme_public_value *owned = eval_ensure_owned(ev, item, &code);
  if (owned == NULL)
    return code;
  *out = owned;
  return LEME_CONTROL_OK;
}

enum leme_control_code
eval_collection_call(struct evaluator *ev, const struct leme_control_node *node,
                     const struct leme_public_value **out) {
  const struct leme_control_operator *op = node->as.call.op;
  switch (op->opcode) {
  case LEME_CONTROL_OP_WHERE:
    return eval_where(ev, node, out);
  case LEME_CONTROL_OP_SELECT:
    return eval_select(ev, node, out);
  case LEME_CONTROL_OP_MAP:
    return eval_map(ev, node, out);
  case LEME_CONTROL_OP_SORT_BY:
    return eval_sort_by(ev, node, out);
  case LEME_CONTROL_OP_LIMIT:
    return eval_limit(ev, node, out);
  case LEME_CONTROL_OP_FIRST:
    return eval_first(ev, node, out);
  default:
    return eval_set_error(ev, "/expr", LEME_CONTROL_UNSUPPORTED,
                          "unsupported collection operator");
  }
}
