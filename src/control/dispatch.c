#include "control/dispatch.h"
#include "control/action.h"
#include "control/action-internal.h"
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
#include "public/budget.h"
#include "public/model.h"
#include "public/schema.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

static bool charge_json(void *context, size_t units) {
  return leme_control_charge(context, units) == LEME_CONTROL_OK;
}

struct capture_meter {
  struct leme_control_meter *meter;
  const char *failure;
};

static enum leme_public_status charge_public(void *context, size_t units) {
  struct capture_meter *capture = context;
  const size_t chunks = units / 128 + (units % 128 != 0 ? 1u : 0u);
  const bool insufficient = capture->meter->remaining < chunks;
  if (leme_control_charge(capture->meter, chunks) == LEME_CONTROL_OK)
    return LEME_PUBLIC_OK;
  if (capture->failure == NULL)
    capture->failure = insufficient ? "work" : "deadline";
  return LEME_PUBLIC_LIMIT;
}

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
                      enum leme_public_status status,
                      const struct capture_meter *capture) {
  const int written =
      snprintf(error->message, sizeof(error->message),
               "snapshot capture failed: stage=%s root=%s status=%s budget=%s",
               diagnostic->stage, capture_root_name(diagnostic->root),
               capture_status_name(status),
               capture->failure == NULL ? "none" : capture->failure);
  if (written < 0 || (size_t)written >= sizeof(error->message)) {
    memcpy(error->message, "snapshot capture failed",
           sizeof("snapshot capture failed"));
  }
}

static const char *outcome_to_str(enum leme_control_outcome outcome) {
  switch (outcome) {
  case LEME_CONTROL_APPLIED:
    return "applied";
  case LEME_CONTROL_ACCEPTED:
    return "accepted";
  case LEME_CONTROL_NOOP:
    return "noop";
  case LEME_CONTROL_FAILED:
    return "failed";
  case LEME_CONTROL_UNATTEMPTED:
    return "unattempted";
  }
  return "unattempted";
}

static void write_results_json(const struct leme_control_plan *plan,
                               struct leme_json *json) {
  leme_json_array_begin(json);
  for (size_t i = 0; i < plan->count; ++i) {
    const struct leme_control_intent *intent = &plan->intents[i];
    leme_json_object_begin(json);
    if (intent->has_target) {
      leme_json_key(json, "target");
      if (intent->effective_id.data != NULL) {
        leme_json_string_n(json, intent->effective_id.data,
                           intent->effective_id.length);
      } else {
        leme_json_null(json);
      }
    } else {
      leme_json_key(json, "target");
      leme_json_null(json);
      const struct leme_control_operator *op_info =
          leme_control_operator_by_opcode(intent->opcode);
      struct leme_public_text op_name =
          op_info != NULL ? op_info->name : LEME_PUBLIC_TEXT("unknown");
      leme_json_key(json, "operation");
      leme_json_string_n(json, op_name.data, op_name.length);
    }
    leme_json_key(json, "requested_targets");
    leme_json_array_begin(json);
    for (size_t r = 0; r < intent->requested_count; ++r) {
      if (intent->requested_targets != NULL &&
          intent->requested_targets[r].data != NULL) {
        leme_json_string_n(json, intent->requested_targets[r].data,
                           intent->requested_targets[r].length);
      } else {
        leme_json_null(json);
      }
    }
    leme_json_array_end(json);

    enum leme_control_outcome outcome =
        plan->outcomes != NULL ? plan->outcomes[i] : LEME_CONTROL_UNATTEMPTED;
    leme_json_key(json, "status");
    leme_json_string(json, outcome_to_str(outcome));

    leme_json_object_end(json);
  }
  leme_json_array_end(json);
}

static void plan_write_value_json(const struct leme_control_plan *plan,
                                  const char *warning_code,
                                  struct leme_json *json) {
  leme_json_object_begin(json);
  leme_json_key(json, "selection_revision");
  if (plan->selection_revision.data != NULL &&
      plan->selection_revision.length > 0) {
    leme_json_string_n(json, plan->selection_revision.data,
                       plan->selection_revision.length);
  } else {
    leme_json_null(json);
  }
  leme_json_key(json, "results");
  write_results_json(plan, json);
  leme_json_key(json, "affected");
  leme_json_integer(json, (int64_t)plan->affected);
  leme_json_key(json, "warnings");
  if (warning_code != NULL) {
    leme_json_array_begin(json);
    leme_json_object_begin(json);
    leme_json_key(json, "code");
    leme_json_string(json, warning_code);
    leme_json_object_end(json);
    leme_json_array_end(json);
  } else {
    leme_json_array_begin(json);
    leme_json_array_end(json);
  }
  leme_json_object_end(json);
}

static void plan_write_details_json(const struct leme_control_plan *plan,
                                    struct leme_json *json) {
  leme_json_object_begin(json);
  leme_json_key(json, "effects_applied");
  leme_json_bool(json, leme_control_plan_effects_applied(plan));
  leme_json_key(json, "selection_revision");
  if (plan->selection_revision.data != NULL &&
      plan->selection_revision.length > 0) {
    leme_json_string_n(json, plan->selection_revision.data,
                       plan->selection_revision.length);
  } else {
    leme_json_null(json);
  }
  leme_json_key(json, "results");
  write_results_json(plan, json);
  leme_json_object_end(json);
}

static bool serialize_action_reply(
    struct leme_control_frame *frame,
    struct leme_public_text req_id,
    struct leme_public_text inst,
    const char *rev_data, size_t rev_len,
    bool ok,
    const char *warning_code,
    const struct leme_control_error *error,
    const struct leme_control_plan *plan) {
  size_t capacity = 0;
  char *buf = leme_control_frame_buffer(frame, &capacity);
  if (buf == NULL || capacity == 0) {
    return false;
  }
  struct leme_json json;
  leme_json_init_fixed(&json, buf, capacity);
  leme_json_object_begin(&json);

  leme_json_key(&json, "type");
  leme_json_string(&json, "reply");

  leme_json_key(&json, "id");
  if (req_id.data != NULL) {
    leme_json_string_n(&json, req_id.data, req_id.length);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "ok");
  leme_json_bool(&json, ok);

  leme_json_key(&json, "instance");
  if (inst.data != NULL) {
    leme_json_string_n(&json, inst.data, inst.length);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "revision");
  if (rev_data != NULL) {
    leme_json_string_n(&json, rev_data, rev_len);
  } else {
    leme_json_null(&json);
  }

  if (ok) {
    leme_json_key(&json, "value");
    plan_write_value_json(plan, warning_code, &json);
  } else {
    enum leme_control_code err_code =
        error != NULL ? error->code : LEME_CONTROL_INVALID_REQUEST;
    const char *msg = (error != NULL && error->message[0] != '\0')
                          ? error->message
                          : leme_control_code_to_string(err_code);
    const char *phase_str =
        (error != NULL && error->phase == LEME_CONTROL_PREFLIGHT) ? "preflight"
                                                                  : "execute";
    leme_json_key(&json, "error");
    leme_json_object_begin(&json);

    leme_json_key(&json, "code");
    leme_json_string(&json, leme_control_code_to_string(err_code));

    leme_json_key(&json, "message");
    leme_json_string(&json, msg);

    leme_json_key(&json, "phase");
    leme_json_string(&json, phase_str);

    if (error != NULL && error->expr_path[0] != '\0') {
      leme_json_key(&json, "expr_path");
      leme_json_string(&json, error->expr_path);
    }

    leme_json_key(&json, "details");
    plan_write_details_json(plan, &json);

    leme_json_object_end(&json);
  }

  leme_json_object_end(&json);
  leme_json_append(&json, "\n", 1);

  if (json.failed) {
    return false;
  }
  leme_control_frame_set_length(frame, json.length);
  return true;
}

static enum leme_control_code dispatch_request(
    struct leme_control_context *context,
    struct leme_control_peer *peer,
    const struct leme_control_request *request,
    struct leme_control_frame **out_frame) {
  if (context == NULL || request == NULL || out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_limits *limits = leme_control_context_limits(context);
  struct leme_control_meter meter = {
      .remaining = limits->work_units,
      .deadline_ns = leme_control_context_deadline(context),
      .now_ns = monotonic_now_ns,
  };
  struct capture_meter capture = {.meter = &meter};
  const struct leme_public_work work = {.context = &capture,
                                        .step = charge_public};
  struct leme_public_text req_id = leme_control_request_id(request);
  enum leme_control_request_op op = leme_control_request_operation(request);
  struct leme_public_model *model = leme_control_context_model(context);
  struct leme_public_text inst = leme_public_model_instance(model);

  if (op == LEME_CONTROL_HELLO) {
    struct leme_public_builder *b = NULL;
    enum leme_public_status b_st =
        leme_public_builder_create_budget(account, limits->response_bytes, &b);
    if (b_st != LEME_PUBLIC_OK) {
      struct leme_control_error err = {
          .code = LEME_CONTROL_OUT_OF_MEMORY,
          .phase = LEME_CONTROL_EVALUATE,
          .message = "out of memory",
      };
      return leme_control_reply_create_error(account, limits->response_bytes,
                                             req_id.data, req_id.length,
                                             inst.data, inst.length,
                                             NULL, 0,
                                             &err, out_frame);
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
    leme_public_object(b, 15, &lim_val);
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
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("field_depth"),
                        (int64_t)limits->field_depth);
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
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("snapshot_bytes"),
                        (int64_t)limits->snapshot_bytes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("total_bytes"),
                        (int64_t)limits->total_bytes);
    leme_public_put_int(b, lim_val, LEME_PUBLIC_TEXT("deadline_ns"),
                        (int64_t)limits->deadline_ns);

    struct leme_public_value *hello_val = NULL;
    leme_public_object(b, 4, &hello_val);
    leme_public_put_int(b, hello_val, LEME_PUBLIC_TEXT("api_version"), 1);
    leme_public_put_cstr(b, hello_val, LEME_PUBLIC_TEXT("version"),
                         LEME_PUBLIC_BUILD_VERSION);
    leme_public_object_set(b, hello_val, LEME_PUBLIC_TEXT("capabilities"), caps);
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
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &error, out_frame);
    }

    enum leme_control_code code = leme_control_reply_create_value_metered(
        account, limits->response_bytes,
        req_id.data, req_id.length,
        inst.data, inst.length,
        NULL, 0,
        hello_val, &meter,
        out_frame);

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
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
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
      enum leme_public_status st = leme_public_model_capture_work_diagnostic(
          model, source, roots, &work, &diagnostic, &snap);
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
          capture_error_message(&err, &diagnostic, st, &capture);
          (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
        }
        const bool sensitive = (st != LEME_PUBLIC_LOCKED) &&
                               ((roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0);
        enum leme_control_code rep_code = leme_control_reply_create_error(
            account, limits->response_bytes, req_id.data, req_id.length,
            inst.data, inst.length, NULL, 0, &err, out_frame);
        if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
          leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_QUERY,
                                          sensitive, req_id.data, req_id.length);
        }
        return rep_code;
      }
    }

    struct leme_control_evaluation *eval = NULL;
    code = leme_control_evaluate(context, prog, snap, &eval, &err);
    if (code != LEME_CONTROL_OK) {
      const bool sensitive = (roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0;
      size_t rev_len = 0;
      const char *rev_str = NULL;
      if (sensitive && snap != NULL) {
        struct leme_public_text rev_text = leme_public_snapshot_revision(snap);
        rev_str = rev_text.data;
        rev_len = rev_text.length;
      }
      enum leme_control_code rep_code = leme_control_reply_create_error(
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, rev_str, rev_len, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
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
    size_t rev_len = 0;
    const char *rev_str = NULL;
    if (sensitive && snap != NULL) {
      struct leme_public_text rev_text = leme_public_snapshot_revision(snap);
      rev_str = rev_text.data;
      rev_len = rev_text.length;
    }

    code = leme_control_reply_create_value_metered(
        account, limits->response_bytes,
        req_id.data, req_id.length,
        inst.data, inst.length,
        rev_str, rev_len,
        val, &meter, out_frame);
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
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
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
    enum leme_public_status st = leme_public_model_capture_work_diagnostic(
        model, source, roots, &work, &diagnostic, &snap);
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
        capture_error_message(&err, &diagnostic, st, &capture);
        (void)snprintf(err.expr_path, sizeof(err.expr_path), "/expr");
      }
      enum leme_control_code rep_code = leme_control_reply_create_error(
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    struct leme_control_plan *plan = NULL;
    code = leme_control_prepare_action(context, prog, snap, &plan, &err);
    if (code != LEME_CONTROL_OK) {
      if (snap != NULL) {
        leme_public_snapshot_unref(snap);
      }
      leme_control_program_destroy(prog);
      enum leme_control_code rep_code = leme_control_reply_create_error(
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    struct leme_json trial_json;
    leme_json_init_budget(&trial_json, account, limits->response_bytes);
    leme_json_set_meter(&trial_json, &meter, charge_json);
    leme_json_object_begin(&trial_json);
    leme_json_key(&trial_json, "type");
    leme_json_string(&trial_json, "reply");
    leme_json_key(&trial_json, "id");
    if (req_id.data != NULL) {
      leme_json_string_n(&trial_json, req_id.data, req_id.length);
    } else {
      leme_json_null(&trial_json);
    }
    leme_json_key(&trial_json, "ok");
    leme_json_bool(&trial_json, true);
    leme_json_key(&trial_json, "instance");
    if (inst.data != NULL) {
      leme_json_string_n(&trial_json, inst.data, inst.length);
    } else {
      leme_json_null(&trial_json);
    }
    leme_json_key(&trial_json, "revision");
    leme_json_string(&trial_json, "18446744073709551615");
    leme_json_key(&trial_json, "value");
    plan->affected = plan->count;
    plan_write_value_json(plan, "publication_failed", &trial_json);
    plan->affected = 0;
    leme_json_object_end(&trial_json);

    bool succ_failed = trial_json.failed;
    size_t succ_len = trial_json.length;
    enum leme_json_error succ_err = trial_json.error;
    leme_json_finish(&trial_json);

    struct leme_json err_trial;
    leme_json_init_budget(&err_trial, account, limits->response_bytes);
    leme_json_set_meter(&err_trial, &meter, charge_json);
    leme_json_object_begin(&err_trial);
    leme_json_key(&err_trial, "type");
    leme_json_string(&err_trial, "reply");
    leme_json_key(&err_trial, "id");
    if (req_id.data != NULL) {
      leme_json_string_n(&err_trial, req_id.data, req_id.length);
    } else {
      leme_json_null(&err_trial);
    }
    leme_json_key(&err_trial, "ok");
    leme_json_bool(&err_trial, false);
    leme_json_key(&err_trial, "instance");
    if (inst.data != NULL) {
      leme_json_string_n(&err_trial, inst.data, inst.length);
    } else {
      leme_json_null(&err_trial);
    }
    leme_json_key(&err_trial, "revision");
    leme_json_string(&err_trial, "18446744073709551615");
    leme_json_key(&err_trial, "error");
    leme_json_object_begin(&err_trial);
    leme_json_key(&err_trial, "code");
    leme_json_string(&err_trial, "unsupported_version");
    leme_json_key(&err_trial, "message");
    char message_bound[sizeof(err.message) - 1];
    memset(message_bound, 1, sizeof(message_bound));
    leme_json_string_n(&err_trial, message_bound, sizeof(message_bound));
    leme_json_key(&err_trial, "phase");
    leme_json_string(&err_trial, "execute");
    leme_json_key(&err_trial, "expr_path");
    char path_bound[sizeof(err.expr_path) - 1];
    memset(path_bound, 1, sizeof(path_bound));
    leme_json_string_n(&err_trial, path_bound, sizeof(path_bound));
    leme_json_key(&err_trial, "details");
    plan_write_details_json(plan, &err_trial);
    leme_json_object_end(&err_trial);
    leme_json_object_end(&err_trial);

    bool err_failed = err_trial.failed;
    size_t err_len = err_trial.length;
    enum leme_json_error err_json_err = err_trial.error;
    leme_json_finish(&err_trial);

    if (succ_failed || err_failed) {
      leme_control_plan_destroy(plan);
      if (snap != NULL) {
        leme_public_snapshot_unref(snap);
      }
      leme_control_program_destroy(prog);
      enum leme_json_error f_err = succ_failed ? succ_err : err_json_err;
      struct leme_control_error mem_err = {
          .code = (f_err == LEME_JSON_ERROR_LIMIT)
                      ? LEME_CONTROL_RESOURCE_LIMIT
                      : LEME_CONTROL_OUT_OF_MEMORY,
          .phase = LEME_CONTROL_PREFLIGHT,
          .message = "failed to build result",
          .effects_applied = false,
      };
      (void)snprintf(mem_err.expr_path, sizeof(mem_err.expr_path), "/expr");
      enum leme_control_code rep_code = leme_control_reply_create_error(
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &mem_err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    size_t max_len = succ_len > err_len ? succ_len : err_len;
    size_t frame_capacity = max_len + 2;

    struct leme_control_frame *reserved_frame = NULL;
    enum leme_control_code frame_code =
        leme_control_frame_create_capacity(account, frame_capacity, &reserved_frame);
    if (frame_code == LEME_CONTROL_OK && peer != NULL) {
      frame_code = leme_control_peer_reserve_reply(peer, frame_capacity - 1);
    }
    if (frame_code != LEME_CONTROL_OK || reserved_frame == NULL) {
      leme_control_frame_destroy(reserved_frame);
      leme_control_plan_destroy(plan);
      if (snap != NULL) {
        leme_public_snapshot_unref(snap);
      }
      leme_control_program_destroy(prog);
      struct leme_control_error mem_err = {
          .code = frame_code,
          .phase = LEME_CONTROL_PREFLIGHT,
          .message = "failed to allocate response buffer",
          .effects_applied = false,
      };
      (void)snprintf(mem_err.expr_path, sizeof(mem_err.expr_path), "/expr");
      enum leme_control_code rep_code = leme_control_reply_create_error(
          account, limits->response_bytes, req_id.data, req_id.length,
          inst.data, inst.length, NULL, 0, &mem_err, out_frame);
      if (rep_code == LEME_CONTROL_OK && out_frame != NULL && *out_frame != NULL) {
        leme_control_frame_set_metadata(*out_frame, LEME_CONTROL_FRAME_ACTION,
                                        false, req_id.data, req_id.length);
      }
      return rep_code;
    }

    code = leme_control_execute_action(context, plan, &err);
    if (code != LEME_CONTROL_OK) {
      if (snap != NULL) {
        leme_public_snapshot_unref(snap);
      }
      leme_control_program_destroy(prog);

      struct leme_public_snapshot *post_snap = NULL;
      const char *post_rev_data = NULL;
      size_t post_rev_len = 0;
      if (leme_control_plan_effects_applied(plan)) {
        leme_public_model_invalidate(model);
        if (leme_public_model_capture_work(model, source, roots, &work, &post_snap) ==
            LEME_PUBLIC_OK) {
          struct leme_public_text post_rev =
              leme_public_snapshot_revision(post_snap);
          post_rev_data = post_rev.data;
          post_rev_len = post_rev.length;
        }
      }

      if (!serialize_action_reply(reserved_frame, req_id, inst,
                                  post_rev_data, post_rev_len,
                                  false, NULL, &err, plan)) {
        leme_control_frame_destroy(reserved_frame);
        reserved_frame = NULL;
      }
      if (reserved_frame != NULL) {
        leme_control_frame_set_metadata(reserved_frame, LEME_CONTROL_FRAME_ACTION,
                                        true, req_id.data, req_id.length);
      }
      if (post_snap != NULL) {
        leme_public_snapshot_unref(post_snap);
      }
      leme_control_plan_destroy(plan);
      *out_frame = reserved_frame;
      return reserved_frame != NULL ? LEME_CONTROL_OK : LEME_CONTROL_RESOURCE_LIMIT;
    }

    if (leme_control_plan_effects_applied(plan))
      leme_public_model_invalidate(model);
    const char *warning_code = NULL;
    struct leme_public_snapshot *post_snap = NULL;
    enum leme_public_status post_st =
        leme_public_model_capture_work(model, source, roots, &work, &post_snap);
    const char *rev_data = NULL;
    size_t rev_len = 0;
    if (post_st != LEME_PUBLIC_OK) {
      leme_public_model_invalidate(model);
      warning_code = "publication_failed";
    } else {
      struct leme_public_text post_rev =
          leme_public_snapshot_revision(post_snap);
      rev_data = post_rev.data;
      rev_len = post_rev.length;
    }

    if (!serialize_action_reply(reserved_frame, req_id, inst,
                                rev_data, rev_len,
                                true, warning_code, NULL, plan)) {
      leme_control_frame_destroy(reserved_frame);
      reserved_frame = NULL;
    }
    if (reserved_frame != NULL) {
      leme_control_frame_set_metadata(reserved_frame, LEME_CONTROL_FRAME_ACTION,
                                      true, req_id.data, req_id.length);
    }
    if (post_snap != NULL) {
      leme_public_snapshot_unref(post_snap);
    }
    if (snap != NULL) {
      leme_public_snapshot_unref(snap);
    }
    leme_control_plan_destroy(plan);
    leme_control_program_destroy(prog);
    *out_frame = reserved_frame;
    return reserved_frame != NULL ? LEME_CONTROL_OK : LEME_CONTROL_RESOURCE_LIMIT;
  }

  (void)peer;
  struct leme_control_error err = {
      .code = LEME_CONTROL_UNSUPPORTED,
      .phase = LEME_CONTROL_VALIDATE,
      .message = "action unsupported",
  };
  (void)snprintf(err.expr_path, sizeof(err.expr_path), "/op");
  return leme_control_reply_create_error(account, limits->response_bytes,
                                         req_id.data, req_id.length,
                                         inst.data, inst.length, NULL, 0,
                                         &err, out_frame);
}

enum leme_control_code leme_control_dispatch_request(
    struct leme_control_context *context, struct leme_control_peer *peer,
    const struct leme_control_request *request,
    struct leme_control_frame **out_frame) {
  if (context == NULL || request == NULL || out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  const uint64_t previous = leme_control_context_deadline(context);
  const struct leme_control_limits *limits = leme_control_context_limits(context);
  if (previous == 0 && limits->deadline_ns != 0) {
    const uint64_t now = monotonic_now_ns(NULL);
    const uint64_t deadline = limits->deadline_ns > UINT64_MAX - now
        ? UINT64_MAX : now + limits->deadline_ns;
    leme_control_context_set_deadline(context, deadline);
  }
  const enum leme_control_code code =
      dispatch_request(context, peer, request, out_frame);
  leme_control_context_set_deadline(context, previous);
  return code;
}
