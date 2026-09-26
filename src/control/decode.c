#include "control/decode.h"
#include "control/memory.h"
#include "control/number.h"
#include "public/value-internal.h"
#include "yyjson.h"

#include <errno.h>
#include <string.h>

struct leme_control_document {
  struct leme_public_builder *builder;
  const struct leme_public_value *root;
};

static void *yy_malloc(void *ctx, size_t size) {
  struct leme_public_budget *account = ctx;
  return leme_control_alloc(account, size);
}

static void *yy_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
  (void)ctx;
  (void)old_size;
  return leme_control_realloc(ptr, size);
}

static void yy_free(void *ctx, void *ptr) {
  (void)ctx;
  leme_control_free(ptr);
}

static enum leme_public_status number_work(void *context, size_t units) {
  struct leme_control_meter *meter = context;
  return meter == NULL || leme_control_charge(meter, units) == LEME_CONTROL_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

static bool prescan_json(const char *data, size_t len, size_t max_depth) {
  size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  bool top_level_closed = false;

  for (size_t i = 0; i < len; i++) {
    const char c = data[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
        if (depth == 0)
          top_level_closed = true;
      } else if ((unsigned char)c < 0x20) {
        return false;
      }
    } else {
      if (top_level_closed) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
          return false;
      } else if (c == '"') {
        in_string = true;
      } else if (c == '[' || c == '{') {
        depth++;
        if (depth > max_depth)
          return false;
      } else if (c == ']' || c == '}') {
        if (depth == 0)
          return false;
        depth--;
        if (depth == 0)
          top_level_closed = true;
      }
    }
  }
  return !in_string && depth == 0;
}

static enum leme_control_code convert_node(struct leme_public_builder *b,
                                           yyjson_val *val,
                                           struct leme_public_value **out,
                                           struct leme_control_meter *meter) {
  if (meter != NULL) {
    const enum leme_control_code charge_code = leme_control_charge(meter, 1);
    if (charge_code != LEME_CONTROL_OK)
      return charge_code;
  }

  if (yyjson_is_null(val)) {
    if (leme_public_null(b, out) != LEME_PUBLIC_OK)
      return LEME_CONTROL_OUT_OF_MEMORY;
    return LEME_CONTROL_OK;
  }

  if (yyjson_is_bool(val)) {
    if (leme_public_boolean(b, yyjson_get_bool(val), out) != LEME_PUBLIC_OK)
      return LEME_CONTROL_OUT_OF_MEMORY;
    return LEME_CONTROL_OK;
  }

  if (yyjson_is_raw(val)) {
    const char *raw = yyjson_get_raw(val);
    const size_t len = yyjson_get_len(val);
    double num = 0.0;
    const struct leme_public_allocator allocator = {
        .context = b->budget, .allocate = yy_malloc, .release = yy_free};
    const struct leme_public_work work = {.context = meter,
                                          .step = number_work};
    const enum leme_public_status parsed = leme_control_parse_number_checked(
        (struct leme_public_text){raw, len}, &allocator, &work, &num);
    if (parsed != LEME_PUBLIC_OK)
      return parsed == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
             : parsed == LEME_PUBLIC_OOM ? LEME_CONTROL_OUT_OF_MEMORY
                                         : LEME_CONTROL_INVALID_JSON;
    if (leme_public_number(b, num, out) != LEME_PUBLIC_OK)
      return LEME_CONTROL_INVALID_JSON;
    return LEME_CONTROL_OK;
  }

  if (yyjson_is_str(val)) {
    const char *str = yyjson_get_str(val);
    const size_t len = yyjson_get_len(val);
    if (leme_public_string(b, (struct leme_public_text){str, len}, false,
                           out) != LEME_PUBLIC_OK)
      return LEME_CONTROL_INVALID_JSON;
    return LEME_CONTROL_OK;
  }

  if (yyjson_is_arr(val)) {
    const size_t count = yyjson_arr_size(val);
    struct leme_public_value *arr = NULL;
    if (leme_public_array(b, count, &arr) != LEME_PUBLIC_OK)
      return LEME_CONTROL_OUT_OF_MEMORY;
    size_t idx = 0;
    yyjson_val *item = NULL;
    yyjson_arr_iter iter = {0};
    yyjson_arr_iter_init(val, &iter);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL) {
      struct leme_public_value *child = NULL;
      const enum leme_control_code code = convert_node(b, item, &child, meter);
      if (code != LEME_CONTROL_OK)
        return code;
      if (leme_public_array_set(b, arr, idx++, child) != LEME_PUBLIC_OK)
        return LEME_CONTROL_INVALID_JSON;
    }
    *out = arr;
    return LEME_CONTROL_OK;
  }

  if (yyjson_is_obj(val)) {
    const size_t count = yyjson_obj_size(val);
    struct leme_public_value *obj = NULL;
    if (leme_public_object(b, count, &obj) != LEME_PUBLIC_OK)
      return LEME_CONTROL_OUT_OF_MEMORY;
    if (count == 0) {
      *out = obj;
      return LEME_CONTROL_OK;
    }

    size_t idx = 0;
    yyjson_val *key_val = NULL;
    yyjson_obj_iter iter = {0};
    yyjson_obj_iter_init(val, &iter);
    while ((key_val = yyjson_obj_iter_next(&iter)) != NULL) {
      yyjson_val *item_val = yyjson_obj_iter_get_val(key_val);
      const char *key_str = yyjson_get_str(key_val);
      const size_t key_len = yyjson_get_len(key_val);

      if (key_len == SIZE_MAX || idx > SIZE_MAX / (key_len + 1) ||
          leme_control_charge(meter, idx * (key_len + 1)) != LEME_CONTROL_OK)
        return LEME_CONTROL_RESOURCE_LIMIT;

      struct leme_public_value *child = NULL;
      const enum leme_control_code code =
          convert_node(b, item_val, &child, meter);
      if (code != LEME_CONTROL_OK)
        return code;
      if (leme_public_object_set(b, obj,
                                 (struct leme_public_text){key_str, key_len},
                                 child) != LEME_PUBLIC_OK)
        return LEME_CONTROL_INVALID_JSON;
      idx++;
    }
    *out = obj;
    return LEME_CONTROL_OK;
  }

  return LEME_CONTROL_INVALID_JSON;
}

enum leme_control_code leme_control_decode(
    struct leme_public_budget *account, struct leme_public_text bytes,
    const struct leme_control_limits *limits, struct leme_control_meter *meter,
    struct leme_control_document **out, struct leme_control_error *error) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (account == NULL || limits == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  if (bytes.length == SIZE_MAX || bytes.length > limits->request_bytes) {
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = LEME_CONTROL_INVALID_JSON,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return LEME_CONTROL_INVALID_JSON;
  }

  if (meter != NULL) {
    const size_t units = bytes.length / 128 + (bytes.length % 128 != 0);
    const enum leme_control_code charge_code =
        leme_control_charge(meter, units > 0 ? units : 1);
    if (charge_code != LEME_CONTROL_OK) {
      if (error != NULL) {
        *error = (struct leme_control_error){
            .code = charge_code,
            .phase = LEME_CONTROL_DECODE,
        };
      }
      return charge_code;
    }
  }

  if (!prescan_json(bytes.data, bytes.length, limits->json_depth)) {
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = LEME_CONTROL_INVALID_JSON,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return LEME_CONTROL_INVALID_JSON;
  }

  char *copy = leme_control_alloc(account, bytes.length + 1);
  if (copy == NULL) {
    const enum leme_control_code code = errno == ENOSPC
                                            ? LEME_CONTROL_RESOURCE_LIMIT
                                            : LEME_CONTROL_OUT_OF_MEMORY;
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = code,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return code;
  }
  memcpy(copy, bytes.data, bytes.length);
  copy[bytes.length] = '\0';

  const yyjson_alc alc = {
      .malloc = yy_malloc,
      .realloc = yy_realloc,
      .free = yy_free,
      .ctx = account,
  };

  yyjson_read_err err = {0};
  yyjson_doc *doc = yyjson_read_opts(copy, bytes.length,
                                     YYJSON_READ_NUMBER_AS_RAW, &alc, &err);
  const int parse_errno = errno;
  leme_control_free(copy);

  if (doc == NULL) {
    const bool allocation_failed =
        err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION;
    const enum leme_control_code code =
        allocation_failed ? (parse_errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                                                   : LEME_CONTROL_OUT_OF_MEMORY)
                          : LEME_CONTROL_INVALID_JSON;
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = code,
          .phase = LEME_CONTROL_DECODE,
          .message = "JSON decoding failed",
          .byte_offset = err.pos,
          .has_byte_offset = !allocation_failed,
      };
    }
    return code;
  }

  struct leme_public_builder *b = NULL;
  const enum leme_public_status b_status =
      leme_public_builder_create_budget(account, limits->retained_bytes, &b);
  if (b_status != LEME_PUBLIC_OK) {
    yyjson_doc_free(doc);
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = b_status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                                : LEME_CONTROL_OUT_OF_MEMORY,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return b_status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                         : LEME_CONTROL_OUT_OF_MEMORY;
  }

  const struct leme_public_work work = {.context = meter, .step = number_work};
  if (meter != NULL)
    leme_public_builder_set_work(b, &work);
  yyjson_val *root_val = yyjson_doc_get_root(doc);
  struct leme_public_value *converted_root = NULL;
  enum leme_control_code conv_code =
      convert_node(b, root_val, &converted_root, meter);
  yyjson_doc_free(doc);

  if (conv_code != LEME_CONTROL_OK) {
    const enum leme_public_status status = leme_public_builder_status(b);
    if (status == LEME_PUBLIC_OOM)
      conv_code = LEME_CONTROL_OUT_OF_MEMORY;
    else if (status == LEME_PUBLIC_LIMIT)
      conv_code = LEME_CONTROL_RESOURCE_LIMIT;
    leme_public_builder_destroy(b);
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = conv_code,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return conv_code;
  }

  const struct leme_public_value *roots[1] = {converted_root};
  const enum leme_public_status sealed = leme_public_builder_seal(b, roots, 1);
  if (sealed != LEME_PUBLIC_OK) {
    const enum leme_control_code code =
        sealed == LEME_PUBLIC_OOM     ? LEME_CONTROL_OUT_OF_MEMORY
        : sealed == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                      : LEME_CONTROL_INVALID_JSON;
    leme_public_builder_destroy(b);
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = code,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return code;
  }
  leme_public_builder_set_work(b, NULL);

  struct leme_control_document *doc_out =
      leme_control_alloc(account, sizeof(*doc_out));
  if (doc_out == NULL) {
    const enum leme_control_code code = errno == ENOSPC
                                            ? LEME_CONTROL_RESOURCE_LIMIT
                                            : LEME_CONTROL_OUT_OF_MEMORY;
    leme_public_builder_destroy(b);
    if (error != NULL) {
      *error = (struct leme_control_error){
          .code = code,
          .phase = LEME_CONTROL_DECODE,
      };
    }
    return code;
  }

  *doc_out = (struct leme_control_document){
      .builder = b,
      .root = converted_root,
  };
  *out = doc_out;
  return LEME_CONTROL_OK;
}

const struct leme_public_value *
leme_control_document_value(const struct leme_control_document *document) {
  return document == NULL ? NULL : document->root;
}

struct leme_public_budget *
leme_control_document_account(const struct leme_control_document *document) {
  return document != NULL ? document->builder->budget : NULL;
}

size_t
leme_control_document_bytes(const struct leme_control_document *document) {
  if (document == NULL)
    return 0;
  const size_t arena = leme_public_builder_bytes(document->builder);
  const size_t metadata = leme_control_allocation_bytes(document);
  return arena > SIZE_MAX - metadata ? SIZE_MAX : arena + metadata;
}

size_t leme_control_document_overhead(void) {
  return sizeof(struct leme_control_document) +
         leme_control_allocation_overhead();
}

void leme_control_document_destroy(struct leme_control_document *document) {
  if (document == NULL)
    return;
  struct leme_public_builder *b = document->builder;
  leme_control_free(document);
  leme_public_builder_destroy(b);
}
