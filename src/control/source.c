#include "control/source.h"
#include "control/number.h"
#include "yyjson.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct reader {
  struct leme_public_builder *builder;
  struct leme_public_text text;
  size_t pos;
  struct leme_control_error *error;
};

static enum leme_control_code fail(struct reader *r,
                                   enum leme_control_code code, size_t offset,
                                   const char *message) {
  if (r->error != NULL) {
    *r->error = (struct leme_control_error){
        .code = code,
        .phase = LEME_CONTROL_DECODE,
        .byte_offset = offset,
        .has_byte_offset = true,
    };
    (void)snprintf(r->error->message, sizeof(r->error->message), "%s", message);
    (void)snprintf(r->error->expr_path, sizeof(r->error->expr_path), "/source");
  }
  return code;
}

static enum leme_control_code built(struct reader *r, size_t offset,
                                    enum leme_public_status status) {
  if (status == LEME_PUBLIC_OK)
    return LEME_CONTROL_OK;
  if (status == LEME_PUBLIC_LIMIT)
    return fail(r, LEME_CONTROL_RESOURCE_LIMIT, offset,
                "source exceeds the memory limit");
  return fail(r, LEME_CONTROL_OUT_OF_MEMORY, offset, "out of memory");
}

static enum leme_control_code nest(struct reader *r, size_t level, size_t below,
                                   size_t offset) {
  if (level + below <= LEME_PUBLIC_MAX_DEPTH)
    return LEME_CONTROL_OK;
  return fail(r, LEME_CONTROL_RESOURCE_LIMIT, offset,
              "expression nesting limit exceeded");
}

static bool space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool delimiter(char c) {
  return space(c) || c == '(' || c == ')' || c == ';' || c == '"';
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool at_end(const struct reader *r) { return r->pos >= r->text.length; }

static char peek(const struct reader *r) { return r->text.data[r->pos]; }

static void skip(struct reader *r) {
  while (!at_end(r)) {
    if (space(peek(r)))
      ++r->pos;
    else if (peek(r) == ';')
      while (!at_end(r) && peek(r) != '\n')
        ++r->pos;
    else
      return;
  }
}

static struct leme_public_text token(struct reader *r) {
  const size_t start = r->pos;
  while (!at_end(r) && !delimiter(peek(r)))
    ++r->pos;
  return (struct leme_public_text){r->text.data + start, r->pos - start};
}

static bool same(struct leme_public_text text, const char *word) {
  const size_t length = strlen(word);
  return text.length == length && memcmp(text.data, word, length) == 0;
}

static bool name(struct leme_public_text text) {
  if (text.data[0] == '.' || digit(text.data[0]))
    return false;
  if (text.data[0] == '-' && text.length > 1 && digit(text.data[1]))
    return false;
  return !same(text, "true") && !same(text, "false") && !same(text, "null");
}

static enum leme_control_code wrap(struct reader *r, size_t offset,
                                   const char *key,
                                   const struct leme_public_value *inner,
                                   struct leme_public_value **out) {
  enum leme_public_status status = leme_public_object(r->builder, 1, out);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_object_set(
        r->builder, *out, (struct leme_public_text){key, strlen(key)}, inner);
  return built(r, offset, status);
}

static enum leme_control_code read_string(struct reader *r, size_t level,
                                          struct leme_public_value **out) {
  const size_t start = r->pos;
  const enum leme_control_code nested = nest(r, level, 1, start);
  if (nested != LEME_CONTROL_OK)
    return nested;
  size_t end = start + 1;
  while (end < r->text.length && r->text.data[end] != '"')
    end += r->text.data[end] == '\\' ? 2 : 1;
  if (end >= r->text.length)
    return fail(r, LEME_CONTROL_INVALID_REQUEST, start, "unclosed string");
  r->pos = end + 1;
  yyjson_doc *doc =
      yyjson_read(r->text.data + start, r->pos - start, YYJSON_READ_NOFLAG);
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (root == NULL || !yyjson_is_str(root)) {
    yyjson_doc_free(doc);
    return fail(r, LEME_CONTROL_INVALID_REQUEST, start, "invalid string");
  }
  struct leme_public_value *value = NULL;
  const enum leme_public_status status = leme_public_string(
      r->builder,
      (struct leme_public_text){yyjson_get_str(root), yyjson_get_len(root)},
      false, &value);
  yyjson_doc_free(doc);
  if (status != LEME_PUBLIC_OK)
    return built(r, start, status);
  return wrap(r, start, "literal", value, out);
}

static enum leme_control_code read_field(struct reader *r, size_t level,
                                         size_t start,
                                         struct leme_public_text text,
                                         struct leme_public_value **out) {
  const enum leme_control_code nested = nest(r, level, 2, start);
  if (nested != LEME_CONTROL_OK)
    return nested;
  size_t count = 0;
  for (size_t i = 0; i < text.length; ++i) {
    if (text.data[i] != '.')
      continue;
    if (i + 1 == text.length || text.data[i + 1] == '.')
      return fail(r, LEME_CONTROL_INVALID_REQUEST, start,
                  "field names cannot be empty");
    ++count;
  }
  struct leme_public_value *path = NULL;
  enum leme_public_status status = leme_public_array(r->builder, count, &path);
  size_t index = 0, from = 1;
  for (size_t i = 1; status == LEME_PUBLIC_OK && i <= text.length; ++i) {
    if (i != text.length && text.data[i] != '.')
      continue;
    struct leme_public_value *component = NULL;
    status = leme_public_string(
        r->builder, (struct leme_public_text){text.data + from, i - from},
        false, &component);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_array_set(r->builder, path, index++, component);
    from = i + 1;
  }
  if (status != LEME_PUBLIC_OK)
    return built(r, start, status);
  return wrap(r, start, "field", path, out);
}

static enum leme_control_code read_atom(struct reader *r, size_t level,
                                        struct leme_public_value **out) {
  const size_t start = r->pos;
  const struct leme_public_text text = token(r);
  if (text.data[0] == '.')
    return read_field(r, level, start, text, out);
  const enum leme_control_code nested = nest(r, level, 1, start);
  if (nested != LEME_CONTROL_OK)
    return nested;
  struct leme_public_value *value = NULL;
  enum leme_public_status status = LEME_PUBLIC_OK;
  if (same(text, "true") || same(text, "false"))
    status = leme_public_boolean(r->builder, same(text, "true"), &value);
  else if (same(text, "null"))
    status = leme_public_null(r->builder, &value);
  else if (!name(text)) {
    double number = 0.0;
    if (!leme_control_parse_number(text.data, text.length, &number))
      return fail(r, LEME_CONTROL_INVALID_REQUEST, start, "invalid number");
    status = leme_public_number(r->builder, number, &value);
  } else
    return fail(r, LEME_CONTROL_INVALID_REQUEST, start,
                "names are only valid as operators");
  if (status != LEME_PUBLIC_OK)
    return built(r, start, status);
  return wrap(r, start, "literal", value, out);
}

static enum leme_control_code read_expr(struct reader *r, size_t level,
                                        struct leme_public_value **out);

static enum leme_control_code read_call(struct reader *r, size_t level,
                                        struct leme_public_value **out) {
  const size_t open = r->pos++;
  const enum leme_control_code nested = nest(r, level, 1, open);
  if (nested != LEME_CONTROL_OK)
    return nested;
  skip(r);
  if (at_end(r))
    return fail(r, LEME_CONTROL_INVALID_REQUEST, open, "unclosed list");
  if (peek(r) == ')')
    return fail(r, LEME_CONTROL_INVALID_REQUEST, open,
                "a list needs an operator");
  const size_t head_at = r->pos;
  const struct leme_public_text head = token(r);
  if (head.length == 0 || !name(head))
    return fail(r, LEME_CONTROL_INVALID_REQUEST, head_at,
                "the operator must be a name");
  struct leme_public_value **args = NULL;
  size_t count = 0, capacity = 0;
  enum leme_control_code code = LEME_CONTROL_OK;
  for (;;) {
    skip(r);
    if (at_end(r)) {
      code = fail(r, LEME_CONTROL_INVALID_REQUEST, open, "unclosed list");
      goto cleanup;
    }
    if (peek(r) == ')') {
      ++r->pos;
      break;
    }
    if (count == capacity) {
      const size_t grown = capacity == 0 ? 4 : capacity * 2;
      struct leme_public_value **larger =
          grown > SIZE_MAX / sizeof(*args)
              ? NULL
              : realloc(args, grown * sizeof(*args));
      if (larger == NULL) {
        code = fail(r, LEME_CONTROL_OUT_OF_MEMORY, r->pos, "out of memory");
        goto cleanup;
      }
      args = larger;
      capacity = grown;
    }
    args[count] = NULL;
    code = read_expr(r, level + 2, &args[count]);
    if (code != LEME_CONTROL_OK)
      goto cleanup;
    ++count;
  }
  struct leme_public_value *op = NULL, *list = NULL;
  enum leme_public_status status = leme_public_object(r->builder, 2, out);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_string(r->builder, head, false, &op);
  if (status == LEME_PUBLIC_OK)
    status =
        leme_public_object_set(r->builder, *out, LEME_PUBLIC_TEXT("call"), op);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_array(r->builder, count, &list);
  for (size_t i = 0; status == LEME_PUBLIC_OK && i < count; ++i)
    status = leme_public_array_set(r->builder, list, i, args[i]);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_object_set(r->builder, *out, LEME_PUBLIC_TEXT("args"),
                                    list);
  code = built(r, open, status);
cleanup:
  free(args);
  return code;
}

static enum leme_control_code read_expr(struct reader *r, size_t level,
                                        struct leme_public_value **out) {
  skip(r);
  if (at_end(r))
    return fail(r, LEME_CONTROL_INVALID_REQUEST, r->pos,
                "expected an expression");
  switch (peek(r)) {
  case '(':
    return read_call(r, level, out);
  case ')':
    return fail(r, LEME_CONTROL_INVALID_REQUEST, r->pos, "unexpected ')'");
  case '"':
    return read_string(r, level, out);
  default:
    return read_atom(r, level, out);
  }
}

enum leme_control_code leme_control_source_read(
    struct leme_public_budget *account, struct leme_public_text source,
    const struct leme_control_limits *limits,
    struct leme_public_builder **owner, const struct leme_public_value **out,
    struct leme_control_error *error) {
  *owner = NULL;
  *out = NULL;
  struct reader r = {.text = source, .error = error};
  enum leme_control_code code =
      built(&r, 0,
            leme_public_builder_create_budget(account, limits->retained_bytes,
                                              &r.builder));
  if (code != LEME_CONTROL_OK)
    return code;
  struct leme_public_value *root = NULL;
  code = read_expr(&r, 1, &root);
  if (code == LEME_CONTROL_OK) {
    skip(&r);
    if (!at_end(&r))
      code = fail(&r, LEME_CONTROL_INVALID_REQUEST, r.pos,
                  "expected one expression");
  }
  if (code == LEME_CONTROL_OK) {
    const struct leme_public_value *roots[] = {root};
    code = built(&r, 0, leme_public_builder_seal(r.builder, roots, 1));
  }
  if (code != LEME_CONTROL_OK) {
    leme_public_builder_destroy(r.builder);
    return code;
  }
  *owner = r.builder;
  *out = root;
  return LEME_CONTROL_OK;
}
