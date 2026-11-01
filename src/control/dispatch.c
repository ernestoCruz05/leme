#include "control/dispatch.h"
#include "control/action.h"
#include "control/control.h"
#include "control/error.h"
#include "control/eval.h"
#include "control/expr.h"
#include "control/limits.h"
#include "control/reply.h"
#include "control/request.h"
#include "ipc/connection.h"
#include "ipc/frame.h"
#include "ipc/json.h"
#include "public/model.h"
#include "public/schema.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *capture_root_name(enum leme_public_root root) {
  switch (root) {
  case LEME_PUBLIC_VIEWS:
    return "views";
  case LEME_PUBLIC_TAGS:
    return "tags";
  case LEME_PUBLIC_OUTPUTS:
    return "outputs";
  case LEME_PUBLIC_INPUTS:
    return "inputs";
  case LEME_PUBLIC_SESSION:
    return "session";
  case LEME_PUBLIC_CONFIG:
    return "config";
  case LEME_PUBLIC_RUNTIME:
    return "runtime";
  case LEME_PUBLIC_STATUS:
    return "status";
  case LEME_PUBLIC_ROOT_COUNT:
    return "none";
  }
  return "unknown";
}

static const char *capture_status_name(enum leme_public_status status) {
  switch (status) {
  case LEME_PUBLIC_OK:
    return "ok";
  case LEME_PUBLIC_OOM:
    return "oom";
  case LEME_PUBLIC_LIMIT:
    return "limit";
  case LEME_PUBLIC_INVALID:
    return "invalid";
  case LEME_PUBLIC_UNKNOWN_FIELD:
    return "unknown_field";
  case LEME_PUBLIC_NOT_FOUND:
    return "not_found";
  case LEME_PUBLIC_TYPE_ERROR:
    return "type_error";
  case LEME_PUBLIC_LOCKED:
    return "locked";
  case LEME_PUBLIC_UNAVAILABLE:
    return "unavailable";
  }
  return "unknown";
}

static void
capture_error_message(struct leme_control_error *error,
                      const struct leme_public_capture_diagnostic *diagnostic,
                      enum leme_public_status status) {
  const int written = snprintf(
      error->message, sizeof(error->message),
      "snapshot capture failed: stage=%s root=%s status=%s", diagnostic->stage,
      capture_root_name(diagnostic->root), capture_status_name(status));
  if (written < 0 || (size_t)written >= sizeof(error->message)) {
    memcpy(error->message, "snapshot capture failed",
           sizeof("snapshot capture failed"));
  }
}

static enum leme_control_code act_reply(size_t limit,
                                        struct leme_public_text id,
                                        struct leme_public_text instance,
                                        size_t affected,
                                        struct leme_control_frame **out) {
  struct leme_public_builder *builder = NULL;
  struct leme_public_value *value = NULL;
  enum leme_public_status status =
      leme_public_builder_create(4096, NULL, &builder);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_object(builder, 1, &value);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_int(builder, value, LEME_PUBLIC_TEXT("affected"),
                                 (int64_t)affected);
  if (status == LEME_PUBLIC_OK) {
    const struct leme_public_value *const roots[] = {value};
    status = leme_public_builder_seal(builder, roots, 1);
  }
  const enum leme_control_code code =
      status == LEME_PUBLIC_OK
          ? leme_control_reply_create_value(limit, id.data, id.length,
                                            instance.data, instance.length,
                                            value, out)
          : LEME_CONTROL_OUT_OF_MEMORY;
  leme_public_builder_destroy(builder);
  return code;
}

static enum leme_control_code
dispatch_request(struct leme_control_context *context,
                 struct leme_control_peer *peer,
                 const struct leme_control_request *request,
                 struct leme_control_frame **out_frame) {
  if (context == NULL || request == NULL || out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  struct leme_public_text req_id = leme_control_request_id(request);
  enum leme_control_request_op op = leme_control_request_operation(request);
  struct leme_public_model *model = leme_control_context_model(context);
  struct leme_public_text inst = leme_public_model_instance(model);

  if (op == LEME_CONTROL_HELLO) {
    struct leme_public_builder *b = NULL;
    enum leme_public_status b_st =
        leme_public_builder_create(limits->response_bytes, NULL, &b);
    if (b_st != LEME_PUBLIC_OK) {
      struct leme_control_error err = {
          .code = LEME_CONTROL_OUT_OF_MEMORY,
          .phase = LEME_CONTROL_EVALUATE,
          .message = "out of memory",
      };
      return leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
    }

    struct leme_public_value *caps = NULL;
    leme_public_array(b, 5, &caps);
    struct leme_public_value *cap0 = NULL;
    struct leme_public_value *cap1 = NULL;
    struct leme_public_value *cap2 = NULL;
    struct leme_public_value *cap3 = NULL;
    struct leme_public_value *cap4 = NULL;
    leme_public_string(b, LEME_PUBLIC_TEXT("hello"), false, &cap0);
    leme_public_string(b, LEME_PUBLIC_TEXT("query"), false, &cap1);
    leme_public_string(b, LEME_PUBLIC_TEXT("act"), false, &cap2);
    leme_public_string(b, LEME_PUBLIC_TEXT("watch"), false, &cap3);
    leme_public_string(b, LEME_PUBLIC_TEXT("unwatch"), false, &cap4);
    leme_public_array_set(b, caps, 0, cap0);
    leme_public_array_set(b, caps, 1, cap1);
    leme_public_array_set(b, caps, 2, cap2);
    leme_public_array_set(b, caps, 3, cap3);
    leme_public_array_set(b, caps, 4, cap4);

    struct leme_public_value *lim_val = NULL;
    leme_public_object(b, 11, &lim_val);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("subscriptions"),
                        (int64_t)limits->subscriptions);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("request_bytes"),
                        (int64_t)limits->request_bytes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("response_bytes"),
                        (int64_t)limits->response_bytes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("json_depth"),
                        (int64_t)limits->json_depth);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("expression_depth"),
                        (int64_t)limits->expression_depth);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("expression_nodes"),
                        (int64_t)limits->expression_nodes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("outstanding"),
                        (int64_t)limits->outstanding);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("targets"),
                        (int64_t)limits->targets);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("work_units"),
                        (int64_t)limits->work_units);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("output_bytes"),
                        (int64_t)limits->output_bytes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("retained_bytes"),
                        (int64_t)limits->retained_bytes);

    struct leme_public_value *hello_val = NULL;
    leme_public_object(b, 4, &hello_val);
    leme_public_put_int(b, hello_val, LEME_PUBLIC_TEXT("api_version"), 1);
    leme_public_put_cstr(b, hello_val, LEME_PUBLIC_TEXT("version"),
                         LEME_VERSION);
    leme_public_object_set(b, hello_val, LEME_PUBLIC_TEXT("capabilities"),
                           caps);
    leme_public_object_set(b, hello_val, LEME_PUBLIC_TEXT("limits"), lim_val);

    const struct leme_public_value *const roots[] = {hello_val};
    const enum leme_public_status sealed =
        leme_public_builder_seal(b, roots, 1);
    if (sealed != LEME_PUBLIC_OK) {
      const struct leme_control_error error = {
          .code = sealed == LEME_PUBLIC_OOM ? LEME_CONTROL_OUT_OF_MEMORY
                                            : LEME_CONTROL_RESOURCE_LIMIT,
          .phase = LEME_CONTROL_EVALUATE,
          .message = "hello construction failed"};
      leme_public_builder_destroy(b);
      return leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &error, out_frame);
    }

    enum leme_control_code code = leme_control_reply_create_value(
        limits->response_bytes, req_id.data, req_id.length, inst.data,
        inst.length, hello_val, out_frame);

    leme_public_builder_destroy(b);
    return code;
  }

  if (op == LEME_CONTROL_QUERY) {
    struct leme_control_program *prog = NULL;
    struct leme_control_error err = {0};
    enum leme_control_code code =
        leme_control_compile(context, request, &prog, &err);
    if (code != LEME_CONTROL_OK) {
      enum leme_control_code rep_code = leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL &&
          *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_QUERY,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    uint32_t roots = leme_control_program_roots(prog);
    struct leme_public_snapshot *snap = NULL;
    if (roots != 0) {
      const struct leme_public_source *source =
          leme_control_context_source(context);
      struct leme_public_capture_diagnostic diagnostic = {0};
      enum leme_public_status st = leme_public_model_capture_diagnostic(
          model, source, roots, &diagnostic, &snap);
      if (st != LEME_PUBLIC_OK) {
        leme_control_program_destroy(prog);
        if (st == LEME_PUBLIC_LOCKED) {
          err.code = LEME_CONTROL_SESSION_LOCKED;
          err.phase = LEME_CONTROL_PREFLIGHT;
          (void)snprintf(err.message, sizeof(err.message), "session is locked");
          (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
        } else {
          err.code = LEME_CONTROL_RESOURCE_LIMIT;
          err.phase = LEME_CONTROL_PREFLIGHT;
          capture_error_message(&err, &diagnostic, st);
          (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
        }
        const bool sensitive = (st != LEME_PUBLIC_LOCKED) &&
                               ((roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0);
        enum leme_control_code rep_code = leme_control_reply_create_error(
            limits->response_bytes, req_id.data, req_id.length, inst.data,
            inst.length, &err, out_frame);
        if (rep_code == LEME_CONTROL_OK && out_frame != NULL &&
            *out_frame != NULL) {
          leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_QUERY,
                                          sensitive, req_id.data,
                                          req_id.length);
        }
        return rep_code;
      }
    }

    struct leme_control_evaluation *eval = NULL;
    code = leme_control_evaluate(context, prog, snap, &eval, &err);
    if (code != LEME_CONTROL_OK) {
      const bool sensitive = (roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0;
      enum leme_control_code rep_code = leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL &&
          *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_QUERY,
                                        sensitive, req_id.data, req_id.length);
      }
      if (snap != NULL) {
        leme_public_snapshot_unref(snap);
      }
      leme_control_program_destroy(prog);
      return rep_code;
    }

    const struct leme_public_value *val = leme_control_evaluation_value(eval);
    const bool sensitive = (roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0;

    code = leme_control_reply_create_value(limits->response_bytes, req_id.data,
                                           req_id.length, inst.data,
                                           inst.length, val, out_frame);
    if (code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
      leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_QUERY,
                                      sensitive, req_id.data, req_id.length);
    }

    leme_control_evaluation_destroy(eval);
    if (snap != NULL) {
      leme_public_snapshot_unref(snap);
    }
    leme_control_program_destroy(prog);
    return code;
  }

  if (op == LEME_CONTROL_ACT) {
    struct leme_control_program *prog = NULL;
    struct leme_control_error err = {0};
    enum leme_control_code code =
        leme_control_compile(context, request, &prog, &err);
    if (code != LEME_CONTROL_OK) {
      enum leme_control_code rep_code = leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL &&
          *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    uint32_t roots = leme_control_program_roots(prog);
    if (roots == 0) {
      roots = LEME_PUBLIC_ALL_ROOTS;
    }
    struct leme_public_snapshot *snap = NULL;
    const struct leme_public_source *source =
        leme_control_context_source(context);
    struct leme_public_capture_diagnostic diagnostic = {0};
    enum leme_public_status st = leme_public_model_capture_diagnostic(
        model, source, roots, &diagnostic, &snap);
    if (st != LEME_PUBLIC_OK) {
      leme_control_program_destroy(prog);
      if (st == LEME_PUBLIC_LOCKED) {
        err.code = LEME_CONTROL_SESSION_LOCKED;
        err.phase = LEME_CONTROL_PREFLIGHT;
        (void)snprintf(err.message, sizeof(err.message), "session is locked");
        (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
      } else {
        err.code = LEME_CONTROL_RESOURCE_LIMIT;
        err.phase = LEME_CONTROL_PREFLIGHT;
        capture_error_message(&err, &diagnostic, st);
        (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
      }
      enum leme_control_code rep_code = leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL &&
          *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    size_t affected = 0;
    code = leme_control_act(context, prog, snap, &affected, &err);
    leme_public_snapshot_unref(snap);
    leme_control_program_destroy(prog);
    if (affected > 0)
      leme_public_model_invalidate(model);
    if (code != LEME_CONTROL_OK) {
      err.has_affected = true;
      err.affected = affected;
      code = leme_control_reply_create_error(
          limits->response_bytes, req_id.data, req_id.length, inst.data,
          inst.length, &err, out_frame);
    } else {
      code =
          act_reply(limits->response_bytes, req_id, inst, affected, out_frame);
    }
    if (code == LEME_CONTROL_OK && *out_frame != NULL)
      leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                      true, req_id.data, req_id.length);
    return code;
  }

  (void)peer;
  struct leme_control_error err = {
      .code = LEME_CONTROL_UNSUPPORTED,
      .phase = LEME_CONTROL_VALIDATE,
      .message = "action unsupported",
  };
  (void)snprintf(err.expr_path, sizeof(err.expr_path), "/op");
  return leme_control_reply_create_error(limits->response_bytes, req_id.data,
                                         req_id.length, inst.data, inst.length,
                                         &err, out_frame);
}

enum leme_control_code
leme_control_dispatch_request(struct leme_control_context *context,
                              struct leme_control_peer *peer,
                              const struct leme_control_request *request,
                              struct leme_control_frame **out_frame) {
  if (context == NULL || request == NULL || out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  return dispatch_request(context, peer, request, out_frame);
}
