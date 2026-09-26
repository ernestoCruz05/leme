#include "control/request.h"
#include "control/memory.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct leme_control_request {
  struct leme_control_document *document;
  size_t references;
  enum leme_control_request_op op;
  struct leme_public_text id;
  struct leme_public_text instance;
  struct leme_public_text subscription;
  const struct leme_public_value *expr;
};

static void set_error(struct leme_control_error *error,
                      enum leme_control_code code, const char *msg) {
  if (error == NULL)
    return;
  *error = (struct leme_control_error){
      .code = code,
      .phase = LEME_CONTROL_VALIDATE,
  };
  if (msg != NULL) {
    const size_t len = strlen(msg);
    const size_t max = sizeof(error->message) - 1;
    const size_t copy_len = len < max ? len : max;
    memcpy(error->message, msg, copy_len);
    error->message[copy_len] = '\0';
  }
}

enum leme_control_code
leme_control_request_create(struct leme_control_document **document,
                            struct leme_control_request **out,
                            struct leme_control_error *error) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (document == NULL || *document == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  const struct leme_public_value *root = leme_control_document_value(*document);
  if (root == NULL || leme_public_kind(root) != LEME_PUBLIC_OBJECT) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST,
              "request envelope must be an object");
    return LEME_CONTROL_INVALID_REQUEST;
  }

  const size_t member_count = leme_public_length(root);
  const struct leme_public_value *version_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("version"));
  const struct leme_public_value *id_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("id"));
  const struct leme_public_value *op_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("op"));
  const struct leme_public_value *instance_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("instance"));
  const struct leme_public_value *expr_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("expr"));
  const struct leme_public_value *subscription_val =
      leme_public_get(root, LEME_PUBLIC_TEXT("subscription"));

  if (version_val == NULL) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST, "missing version field");
    return LEME_CONTROL_INVALID_REQUEST;
  }
  double ver_num = 0.0;
  if (leme_public_as_number(version_val, &ver_num) != LEME_PUBLIC_OK ||
      trunc(ver_num) != ver_num) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST,
              "version field must be an integer");
    return LEME_CONTROL_INVALID_REQUEST;
  }
  if ((int64_t)ver_num != 1) {
    set_error(error, LEME_CONTROL_UNSUPPORTED_VERSION,
              "unsupported protocol version");
    return LEME_CONTROL_UNSUPPORTED_VERSION;
  }

  if (id_val == NULL) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST, "missing id field");
    return LEME_CONTROL_INVALID_REQUEST;
  }
  struct leme_public_text id_text = {0};
  if (leme_public_as_text(id_val, &id_text) != LEME_PUBLIC_OK ||
      id_text.length == 0 || id_text.length > 128) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST,
              "id must be a string of 1 to 128 bytes");
    return LEME_CONTROL_INVALID_REQUEST;
  }
  for (size_t i = 0; i < id_text.length; i++) {
    if (id_text.data[i] == '\0') {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "id must not contain embedded NUL");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  }

  if (op_val == NULL) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST, "missing op field");
    return LEME_CONTROL_INVALID_REQUEST;
  }
  struct leme_public_text op_text = {0};
  if (leme_public_as_text(op_val, &op_text) != LEME_PUBLIC_OK) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST, "op must be a string");
    return LEME_CONTROL_INVALID_REQUEST;
  }

  enum leme_control_request_op op = LEME_CONTROL_HELLO;
  if (op_text.length == 5 && memcmp(op_text.data, "hello", 5) == 0) {
    op = LEME_CONTROL_HELLO;
    if (instance_val != NULL || expr_val != NULL || member_count != 3) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "invalid fields in hello request");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  } else if (op_text.length == 5 && memcmp(op_text.data, "query", 5) == 0) {
    op = LEME_CONTROL_QUERY;
    if (instance_val == NULL || expr_val == NULL || member_count != 5) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "invalid fields in query request");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  } else if (op_text.length == 3 && memcmp(op_text.data, "act", 3) == 0) {
    op = LEME_CONTROL_ACT;
    if (instance_val == NULL || expr_val == NULL || member_count != 5) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "invalid fields in act request");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  } else if (op_text.length == 5 && memcmp(op_text.data, "watch", 5) == 0) {
    op = LEME_CONTROL_WATCH;
    if (instance_val == NULL || expr_val == NULL || member_count != 5) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "invalid fields in watch request");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  } else if (op_text.length == 7 && memcmp(op_text.data, "unwatch", 7) == 0) {
    op = LEME_CONTROL_UNWATCH;
    if (instance_val == NULL || subscription_val == NULL || expr_val != NULL ||
        member_count != 5) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "invalid fields in unwatch request");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  } else {
    set_error(error, LEME_CONTROL_INVALID_REQUEST, "unknown request operation");
    return LEME_CONTROL_INVALID_REQUEST;
  }

  struct leme_public_text inst_text = {0};
  if (instance_val != NULL) {
    if (leme_public_as_text(instance_val, &inst_text) != LEME_PUBLIC_OK ||
        inst_text.length == 0) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "instance must be a non-empty string");
      return LEME_CONTROL_INVALID_REQUEST;
    }
    for (size_t i = 0; i < inst_text.length; i++) {
      if (inst_text.data[i] == '\0') {
        set_error(error, LEME_CONTROL_INVALID_REQUEST,
                  "instance must not contain embedded NUL");
        return LEME_CONTROL_INVALID_REQUEST;
      }
    }
  }

  if (expr_val != NULL) {
    if (leme_public_kind(expr_val) != LEME_PUBLIC_OBJECT) {
      set_error(error, LEME_CONTROL_INVALID_REQUEST,
                "expr must be a JSON object");
      return LEME_CONTROL_INVALID_REQUEST;
    }
  }

  struct leme_public_text subscription = {0};
  if (subscription_val != NULL &&
      (leme_public_as_text(subscription_val, &subscription) != LEME_PUBLIC_OK ||
       subscription.length == 0 || subscription.length > 128 ||
       memchr(subscription.data, '\0', subscription.length) != NULL)) {
    set_error(error, LEME_CONTROL_INVALID_REQUEST,
              "subscription must be a NUL-free string of 1 to 128 bytes");
    return LEME_CONTROL_INVALID_REQUEST;
  }

  struct leme_control_request *req = leme_control_alloc(
      leme_control_document_account(*document), sizeof(*req));
  if (req == NULL) {
    const enum leme_control_code code = errno == ENOSPC
                                            ? LEME_CONTROL_RESOURCE_LIMIT
                                            : LEME_CONTROL_OUT_OF_MEMORY;
    set_error(error, code, "request allocation failed");
    return code;
  }
  *req = (struct leme_control_request){
      .document = *document,
      .references = 1,
      .op = op,
      .id = id_text,
      .instance = inst_text,
      .subscription = subscription,
      .expr = expr_val,
  };

  *document = NULL;
  *out = req;
  return LEME_CONTROL_OK;
}

void leme_control_request_ref(struct leme_control_request *request) {
  if (request == NULL)
    return;
  if (request->references == 0 || request->references == SIZE_MAX)
    abort();
  ++request->references;
}

void leme_control_request_destroy(struct leme_control_request *request) {
  if (request == NULL)
    return;
  if (request->references == 0)
    abort();
  if (--request->references != 0)
    return;
  struct leme_control_document *doc = request->document;
  leme_control_free(request);
  leme_control_document_destroy(doc);
}

struct leme_public_text
leme_control_request_id(const struct leme_control_request *request) {
  return request == NULL ? (struct leme_public_text){0} : request->id;
}

struct leme_public_text
leme_control_request_instance(const struct leme_control_request *request) {
  return request == NULL ? (struct leme_public_text){0} : request->instance;
}

struct leme_public_text
leme_control_request_subscription(const struct leme_control_request *request) {
  return request == NULL ? (struct leme_public_text){0} : request->subscription;
}

const struct leme_public_value *
leme_control_request_expression(const struct leme_control_request *request) {
  return request == NULL ? NULL : request->expr;
}

enum leme_control_request_op
leme_control_request_operation(const struct leme_control_request *request) {
  return request == NULL ? LEME_CONTROL_HELLO : request->op;
}
