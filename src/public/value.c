#include "public/value-internal.h"

#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

struct leme_public_node {
  struct leme_public_value value;
  struct leme_public_mark mark;
};

static enum leme_public_status make_value(struct leme_public_builder *b,
                                          enum leme_public_kind kind,
                                          struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_node *node = leme_public_allocate(
      b, 1, sizeof(*node), alignof(struct leme_public_node));
  if (node == NULL) {
    const enum leme_public_status status = leme_public_builder_status(b);
    assert(status != LEME_PUBLIC_OK);
    return status;
  }
  node->value = (struct leme_public_value){
      .owner = b, .next = b->values, .mark = &node->mark, .kind = kind};
  b->values = &node->value;
  *out = &node->value;
  return LEME_PUBLIC_OK;
}

static int compare_text(struct leme_public_text left,
                        struct leme_public_text right) {
  const size_t length = left.length < right.length ? left.length : right.length;
  const int order = length == 0 ? 0 : memcmp(left.data, right.data, length);
  if (order != 0)
    return order;
  return left.length < right.length ? -1 : (left.length > right.length ? 1 : 0);
}

static int compare_members(const void *lhs, const void *rhs) {
  const struct leme_public_member *left = lhs;
  const struct leme_public_member *right = rhs;
  return compare_text(left->key, right->key);
}

enum leme_public_status leme_public_null(struct leme_public_builder *b,
                                         struct leme_public_value **out) {
  return make_value(b, LEME_PUBLIC_NULL, out);
}

enum leme_public_status leme_public_boolean(struct leme_public_builder *b,
                                            bool input,
                                            struct leme_public_value **out) {
  const enum leme_public_status status =
      make_value(b, LEME_PUBLIC_BOOLEAN, out);
  if (status == LEME_PUBLIC_OK)
    (*out)->data.boolean = input;
  return status;
}

enum leme_public_status leme_public_number(struct leme_public_builder *b,
                                           double input,
                                           struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  const double maximum = (double)LEME_PUBLIC_SAFE_INTEGER;
  if (!isfinite(input) ||
      (trunc(input) == input && (input > maximum || input < -maximum))) {
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  }
  const enum leme_public_status status = make_value(b, LEME_PUBLIC_NUMBER, out);
  if (status == LEME_PUBLIC_OK)
    (*out)->data.number = input == 0.0 ? 0.0 : input;
  return status;
}

enum leme_public_status leme_public_integer(struct leme_public_builder *b,
                                            int64_t input,
                                            struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (input < -LEME_PUBLIC_SAFE_INTEGER || input > LEME_PUBLIC_SAFE_INTEGER) {
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  }
  return leme_public_number(b, (double)input, out);
}

static enum leme_public_status container_check(struct leme_public_builder *b,
                                               size_t count, size_t size,
                                               struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  const enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (count > SIZE_MAX / size || count > b->maximum / size) {
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_object(struct leme_public_builder *b,
                                           size_t count,
                                           struct leme_public_value **out) {
  enum leme_public_status status =
      container_check(b, count, sizeof(struct leme_public_member), out);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_value *v = NULL;
  status = make_value(b, LEME_PUBLIC_OBJECT, &v);
  if (status != LEME_PUBLIC_OK)
    return status;
  v->data.object.count = count;
  if (count != 0) {
    v->data.object.members =
        leme_public_allocate(b, count, sizeof(struct leme_public_member),
                             alignof(struct leme_public_member));
    if (v->data.object.members == NULL)
      return leme_public_builder_status(b);
  }
  *out = v;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_array(struct leme_public_builder *b,
                                          size_t count,
                                          struct leme_public_value **out) {
  enum leme_public_status status =
      container_check(b, count, sizeof(const struct leme_public_value *), out);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_value *v = NULL;
  status = make_value(b, LEME_PUBLIC_ARRAY, &v);
  if (status != LEME_PUBLIC_OK)
    return status;
  v->data.array.count = count;
  if (count != 0) {
    v->data.array.items =
        (const struct leme_public_value **)leme_public_allocate(
            b, count, sizeof(const struct leme_public_value *),
            alignof(const struct leme_public_value *));
    if (v->data.array.items == NULL)
      return leme_public_builder_status(b);
  }
  *out = v;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_string(struct leme_public_builder *b,
                                           struct leme_public_text input,
                                           bool repair_utf8,
                                           struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_value *v = NULL;
  enum leme_public_status status = make_value(b, LEME_PUBLIC_STRING, &v);
  if (status != LEME_PUBLIC_OK)
    return status;
  status = leme_public_utf8_copy(b, input, repair_utf8, &v->data.string);
  if (status != LEME_PUBLIC_OK)
    return status;
  *out = v;
  return LEME_PUBLIC_OK;
}

const struct leme_public_value *
leme_public_get(const struct leme_public_value *object,
                struct leme_public_text key) {
  if (object == NULL || object->kind != LEME_PUBLIC_OBJECT ||
      (key.data == NULL && key.length != 0))
    return NULL;
  if (key.length >= object->owner->maximum)
    return NULL;
  if (object->owner->sealed) {
    size_t low = 0;
    size_t high = object->data.object.used;
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      const struct leme_public_member *member =
          &object->data.object.members[middle];
      const int order = compare_text(key, member->key);
      if (order == 0)
        return member->value;
      if (order < 0)
        high = middle;
      else
        low = middle + 1;
    }
    return NULL;
  }
  for (size_t i = 0; i < object->data.object.used; ++i) {
    const struct leme_public_member *member = &object->data.object.members[i];
    if (compare_text(member->key, key) == 0)
      return member->value;
  }
  return NULL;
}

enum leme_public_status leme_public_object_set(
    struct leme_public_builder *b, struct leme_public_value *object,
    struct leme_public_text key, const struct leme_public_value *item) {
  enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (object == NULL || object->owner != b ||
      object->kind != LEME_PUBLIC_OBJECT || item == NULL || item->owner != b ||
      object->data.object.used == object->data.object.count ||
      leme_public_get(object, key) != NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_text owned_key = {0};
  status = leme_public_utf8_copy(b, key, false, &owned_key);
  if (status != LEME_PUBLIC_OK)
    return status;
  object->data.object.members[object->data.object.used++] =
      (struct leme_public_member){.key = owned_key, .value = item};
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_array_set(struct leme_public_builder *b,
                      struct leme_public_value *array, size_t index,
                      const struct leme_public_value *item) {
  const enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (array == NULL || array->owner != b || array->kind != LEME_PUBLIC_ARRAY ||
      index >= array->data.array.count ||
      array->data.array.items[index] != NULL || item == NULL ||
      item->owner != b)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  array->data.array.items[index] = item;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_put_null(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key) {
  struct leme_public_value *v = NULL;
  const enum leme_public_status status = leme_public_null(b, &v);
  return status == LEME_PUBLIC_OK ? leme_public_object_set(b, object, key, v)
                                  : status;
}

enum leme_public_status leme_public_put_bool(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             bool input) {
  struct leme_public_value *v = NULL;
  const enum leme_public_status status = leme_public_boolean(b, input, &v);
  return status == LEME_PUBLIC_OK ? leme_public_object_set(b, object, key, v)
                                  : status;
}

enum leme_public_status leme_public_put_int(struct leme_public_builder *b,
                                            struct leme_public_value *object,
                                            struct leme_public_text key,
                                            int64_t input) {
  struct leme_public_value *v = NULL;
  const enum leme_public_status status = leme_public_integer(b, input, &v);
  return status == LEME_PUBLIC_OK ? leme_public_object_set(b, object, key, v)
                                  : status;
}

enum leme_public_status leme_public_put_number(struct leme_public_builder *b,
                                               struct leme_public_value *object,
                                               struct leme_public_text key,
                                               double input) {
  struct leme_public_value *v = NULL;
  const enum leme_public_status status = leme_public_number(b, input, &v);
  return status == LEME_PUBLIC_OK ? leme_public_object_set(b, object, key, v)
                                  : status;
}

enum leme_public_status leme_public_put_text(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             struct leme_public_text input,
                                             bool repair_utf8) {
  struct leme_public_value *v = NULL;
  const enum leme_public_status status =
      leme_public_string(b, input, repair_utf8, &v);
  return status == LEME_PUBLIC_OK ? leme_public_object_set(b, object, key, v)
                                  : status;
}

enum leme_public_status leme_public_put_cstr(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             const char *input) {
  if (input == NULL)
    return leme_public_put_null(b, object, key);
  const enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  size_t length = 0;
  while (length < b->maximum && input[length] != '\0')
    ++length;
  if (length == b->maximum)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  return leme_public_put_text(
      b, object, key,
      (struct leme_public_text){.data = input, .length = length}, true);
}

enum leme_public_kind leme_public_kind(const struct leme_public_value *value) {
  assert(value != NULL);
  return value->kind;
}

size_t leme_public_length(const struct leme_public_value *value) {
  if (value == NULL)
    return 0;
  switch (value->kind) {
  case LEME_PUBLIC_STRING:
    return value->data.string.length;
  case LEME_PUBLIC_ARRAY:
    return value->data.array.count;
  case LEME_PUBLIC_OBJECT:
    return value->data.object.count;
  default:
    return 0;
  }
}

const struct leme_public_value *
leme_public_at(const struct leme_public_value *array, size_t index) {
  if (array == NULL || array->kind != LEME_PUBLIC_ARRAY ||
      index >= array->data.array.count)
    return NULL;
  return array->data.array.items[index];
}

struct leme_public_text
leme_public_key_at(const struct leme_public_value *object, size_t index) {
  if (object == NULL || object->kind != LEME_PUBLIC_OBJECT ||
      index >= object->data.object.used) {
    return (struct leme_public_text){0};
  }
  return object->data.object.members[index].key;
}

const struct leme_public_value *
leme_public_member_at(const struct leme_public_value *object, size_t index) {
  if (object == NULL || object->kind != LEME_PUBLIC_OBJECT ||
      index >= object->data.object.used)
    return NULL;
  return object->data.object.members[index].value;
}

enum leme_public_status
leme_public_as_text(const struct leme_public_value *value,
                    struct leme_public_text *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = (struct leme_public_text){0};
  if (value == NULL || value->kind != LEME_PUBLIC_STRING)
    return LEME_PUBLIC_TYPE_ERROR;
  *out = value->data.string;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_as_number(const struct leme_public_value *value, double *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = 0.0;
  if (value == NULL || value->kind != LEME_PUBLIC_NUMBER)
    return LEME_PUBLIC_TYPE_ERROR;
  *out = value->data.number;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_as_integer(const struct leme_public_value *value, int64_t *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = 0;
  if (value == NULL || value->kind != LEME_PUBLIC_NUMBER)
    return LEME_PUBLIC_TYPE_ERROR;
  const double number = value->data.number;
  const double maximum = (double)LEME_PUBLIC_SAFE_INTEGER;
  if (!isfinite(number) || trunc(number) != number || number > maximum ||
      number < -maximum) {
    return LEME_PUBLIC_TYPE_ERROR;
  }
  *out = (int64_t)number;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_as_bool(const struct leme_public_value *value, bool *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = false;
  if (value == NULL || value->kind != LEME_PUBLIC_BOOLEAN)
    return LEME_PUBLIC_TYPE_ERROR;
  *out = value->data.boolean;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status check_node(struct leme_public_builder *b,
                                          const struct leme_public_value *value,
                                          size_t depth) {
  if (value == NULL || value->owner != b)
    return LEME_PUBLIC_INVALID;
  struct leme_public_mark *mark = value->mark;
  if (mark->visiting)
    return LEME_PUBLIC_INVALID;
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return LEME_PUBLIC_LIMIT;
  if (mark->height != 0) {
    return mark->height > LEME_PUBLIC_MAX_DEPTH - depth + 1 ? LEME_PUBLIC_LIMIT
                                                            : LEME_PUBLIC_OK;
  }
  if (value->kind == LEME_PUBLIC_OBJECT) {
    if (value->data.object.used != value->data.object.count)
      return LEME_PUBLIC_INVALID;
    if (value->data.object.count > 1) {
      if (b->work.step != NULL) {
        enum leme_public_status step_st =
            b->work.step(b->work.context, value->data.object.count);
        if (step_st != LEME_PUBLIC_OK)
          return step_st;
      }
      qsort(value->data.object.members, value->data.object.count,
            sizeof(struct leme_public_member), compare_members);
    }
  }
  mark->visiting = true;
  if (b->work.step != NULL) {
    enum leme_public_status step_st = b->work.step(b->work.context, 1);
    if (step_st != LEME_PUBLIC_OK)
      return step_st;
  }
  size_t work = 1;
  size_t height = 1;
  if (value->kind == LEME_PUBLIC_STRING) {
    if (value->data.string.length > b->maximum - work)
      return LEME_PUBLIC_LIMIT;
    work += value->data.string.length;
    if (b->work.step != NULL && value->data.string.length > 0) {
      enum leme_public_status step_st =
          b->work.step(b->work.context, value->data.string.length);
      if (step_st != LEME_PUBLIC_OK)
        return step_st;
    }
  }
  const size_t count =
      value->kind == LEME_PUBLIC_ARRAY
          ? value->data.array.count
          : (value->kind == LEME_PUBLIC_OBJECT ? value->data.object.count : 0);
  for (size_t i = 0; i < count; ++i) {
    const struct leme_public_value *child =
        value->kind == LEME_PUBLIC_ARRAY ? value->data.array.items[i]
                                         : value->data.object.members[i].value;
    const enum leme_public_status status = check_node(b, child, depth + 1);
    if (status != LEME_PUBLIC_OK)
      return status;
    if (child->mark->height + 1 > height)
      height = child->mark->height + 1;
    if (child->mark->work > b->maximum - work)
      return LEME_PUBLIC_LIMIT;
    work += child->mark->work;
    if (value->kind == LEME_PUBLIC_OBJECT) {
      const size_t length = value->data.object.members[i].key.length;
      if (length > b->maximum - work)
        return LEME_PUBLIC_LIMIT;
      work += length;
    }
  }
  if (height > LEME_PUBLIC_MAX_DEPTH - depth + 1)
    return LEME_PUBLIC_LIMIT;
  mark->height = height;
  mark->work = work;
  mark->visiting = false;
  return LEME_PUBLIC_OK;
}

void leme_public_builder_set_work(struct leme_public_builder *builder,
                                  const struct leme_public_work *work) {
  if (builder == NULL)
    return;
  builder->work = (work != NULL) ? *work : (struct leme_public_work){0};
}

enum leme_public_status
leme_public_builder_seal(struct leme_public_builder *b,
                         const struct leme_public_value *const *roots,
                         size_t count) {
  enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (roots == NULL && count != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (count > b->maximum)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  for (struct leme_public_value *v = b->values; v != NULL; v = v->next) {
    status = check_node(b, v, 1);
    if (status != LEME_PUBLIC_OK)
      return leme_public_fail(b, status);
  }
  size_t work = 0;
  for (size_t i = 0; i < count; ++i) {
    if (roots[i] == NULL || roots[i]->owner != b)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (roots[i]->mark->work > b->maximum - work)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    work += roots[i]->mark->work;
  }
  b->sealed = true;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
text_equal_checked(struct leme_public_text left, struct leme_public_text right,
                   const struct leme_public_work *work, bool *out) {
  *out = false;
  if (left.length != right.length)
    return LEME_PUBLIC_OK;
  for (size_t offset = 0; offset < left.length;) {
    const size_t remaining = left.length - offset;
    const size_t count = remaining < 128 ? remaining : 128;
    if (work != NULL && work->step != NULL) {
      const enum leme_public_status status = work->step(work->context, count);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    if (memcmp(left.data + offset, right.data + offset, count) != 0)
      return LEME_PUBLIC_OK;
    offset += count;
  }
  *out = true;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status values_equal_checked(
    const struct leme_public_value *left, const struct leme_public_value *right,
    const struct leme_public_work *work, size_t depth, bool *out) {
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return LEME_PUBLIC_LIMIT;
  if (work != NULL && work->step != NULL) {
    enum leme_public_status st = work->step(work->context, 1);
    if (st != LEME_PUBLIC_OK)
      return st;
  }
  if (left == NULL || right == NULL || left->kind != right->kind) {
    *out = false;
    return LEME_PUBLIC_OK;
  }
  if ((left->kind == LEME_PUBLIC_ARRAY || left->kind == LEME_PUBLIC_OBJECT) &&
      (!left->owner->sealed || !right->owner->sealed)) {
    *out = false;
    return LEME_PUBLIC_OK;
  }
  if (left == right) {
    *out = true;
    return LEME_PUBLIC_OK;
  }
  switch (left->kind) {
  case LEME_PUBLIC_NULL:
    *out = true;
    return LEME_PUBLIC_OK;
  case LEME_PUBLIC_BOOLEAN:
    *out = (left->data.boolean == right->data.boolean);
    return LEME_PUBLIC_OK;
  case LEME_PUBLIC_NUMBER:
    *out = (left->data.number == right->data.number);
    return LEME_PUBLIC_OK;
  case LEME_PUBLIC_STRING:
    return text_equal_checked(left->data.string, right->data.string, work, out);
  case LEME_PUBLIC_ARRAY: {
    if (left->data.array.count != right->data.array.count) {
      *out = false;
      return LEME_PUBLIC_OK;
    }
    for (size_t i = 0; i < left->data.array.count; ++i) {
      bool item_equal = false;
      enum leme_public_status st =
          values_equal_checked(left->data.array.items[i],
                               right->data.array.items[i], work, depth + 1,
                               &item_equal);
      if (st != LEME_PUBLIC_OK)
        return st;
      if (!item_equal) {
        *out = false;
        return LEME_PUBLIC_OK;
      }
    }
    *out = true;
    return LEME_PUBLIC_OK;
  }
  case LEME_PUBLIC_OBJECT: {
    if (left->data.object.count != right->data.object.count) {
      *out = false;
      return LEME_PUBLIC_OK;
    }
    for (size_t i = 0; i < left->data.object.count; ++i) {
      const struct leme_public_member *a = &left->data.object.members[i];
      const struct leme_public_member *b = &right->data.object.members[i];
      bool key_equal = false;
      enum leme_public_status st =
          text_equal_checked(a->key, b->key, work, &key_equal);
      if (st != LEME_PUBLIC_OK)
        return st;
      if (!key_equal) {
        *out = false;
        return LEME_PUBLIC_OK;
      }
      bool member_equal = false;
      st = values_equal_checked(a->value, b->value, work, depth + 1,
                                &member_equal);
      if (st != LEME_PUBLIC_OK)
        return st;
      if (!member_equal) {
        *out = false;
        return LEME_PUBLIC_OK;
      }
    }
    *out = true;
    return LEME_PUBLIC_OK;
  }
  }
  *out = false;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_equal_checked(
    const struct leme_public_value *left, const struct leme_public_value *right,
    const struct leme_public_work *work, bool *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  bool result = false;
  enum leme_public_status st =
      values_equal_checked(left, right, work, 1, &result);
  if (st == LEME_PUBLIC_OK)
    *out = result;
  return st;
}

bool leme_public_equal(const struct leme_public_value *left,
                       const struct leme_public_value *right) {
  bool out = false;
  return leme_public_equal_checked(left, right, NULL, &out) == LEME_PUBLIC_OK &&
         out;
}

static enum leme_public_status
clone_value(struct leme_public_builder *b,
            const struct leme_public_value *input, size_t depth,
            struct leme_public_value **out) {
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  if (b->work.step != NULL) {
    enum leme_public_status st = b->work.step(b->work.context, 1);
    if (st != LEME_PUBLIC_OK)
      return leme_public_fail(b, st);
  }
  switch (input->kind) {
  case LEME_PUBLIC_NULL:
    return leme_public_null(b, out);
  case LEME_PUBLIC_BOOLEAN:
    return leme_public_boolean(b, input->data.boolean, out);
  case LEME_PUBLIC_NUMBER:
    return leme_public_number(b, input->data.number, out);
  case LEME_PUBLIC_STRING:
    return leme_public_string(b, input->data.string, false, out);
  case LEME_PUBLIC_ARRAY:
  case LEME_PUBLIC_OBJECT: {
    const size_t count = leme_public_length(input);
    struct leme_public_value *container = NULL;
    enum leme_public_status status =
        input->kind == LEME_PUBLIC_ARRAY
            ? leme_public_array(b, count, &container)
            : leme_public_object(b, count, &container);
    if (status != LEME_PUBLIC_OK)
      return status;
    for (size_t i = 0; i < count; ++i) {
      struct leme_public_value *child = NULL;
      const struct leme_public_value *original =
          input->kind == LEME_PUBLIC_ARRAY ? leme_public_at(input, i)
                                           : leme_public_member_at(input, i);
      status = clone_value(b, original, depth + 1, &child);
      if (status != LEME_PUBLIC_OK)
        return status;
      status = input->kind == LEME_PUBLIC_ARRAY
                   ? leme_public_array_set(b, container, i, child)
                   : leme_public_object_set(
                         b, container, leme_public_key_at(input, i), child);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    *out = container;
    return LEME_PUBLIC_OK;
  }
  }
  return leme_public_fail(b, LEME_PUBLIC_INVALID);
}

enum leme_public_status leme_public_clone(struct leme_public_builder *b,
                                          const struct leme_public_value *input,
                                          struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (input == NULL || !input->owner->sealed)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return clone_value(b, input, 1, out);
}
