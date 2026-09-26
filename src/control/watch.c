#include "control/watch-internal.h"
#include "control/watch-event.h"
#include "control/memory.h"
#include "control/reply.h"
#include "public/json.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static enum leme_control_code fail(struct leme_control_error *error,
                                   enum leme_control_code code,
                                   struct leme_public_text message) {
  if (error != NULL) {
    *error = (struct leme_control_error){.code = code,
                                         .phase = LEME_CONTROL_PREFLIGHT,
                                         .message = "watch operation failed",
                                         .expr_path = "/expr"};
    if (message.length < sizeof(error->message)) {
      memcpy(error->message, message.data, message.length);
      error->message[message.length] = '\0';
    }
  }
  return code;
}

static enum leme_public_status work_step(void *context, size_t units) {
  return leme_control_charge(context, units) == LEME_CONTROL_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

static enum leme_public_status capture_step(void *context, size_t units) {
  const size_t chunks = units / 128 + (units % 128 != 0 ? 1u : 0u);
  return work_step(context, chunks);
}

static struct leme_control_meter
meter_for(const struct leme_control_watch_set *set,
          const struct leme_control_context *context) {
  const uint64_t now = leme_control_now_ns(NULL);
  uint64_t deadline = set->limits.deadline_ns > UINT64_MAX - now
                          ? UINT64_MAX
                          : now + set->limits.deadline_ns;
  const uint64_t outer = leme_control_context_deadline(context);
  if (outer != 0 && outer < deadline)
    deadline = outer;
  return (struct leme_control_meter){.remaining = set->limits.work_units,
                                     .deadline_ns = deadline,
                                     .now_ns = leme_control_now_ns};
}

static void remove_slot(struct leme_control_watch_set *set,
                        struct leme_control_watch_slot *slot) {
  if (slot->state != LEME_WATCH_REMOVED) {
    slot->state = LEME_WATCH_REMOVED;
    assert(set->count > 0);
    --set->count;
  }
  if (slot->busy)
    return;
  leme_control_evaluation_destroy(slot->baseline);
  leme_control_program_destroy(slot->program);
  leme_control_request_destroy(slot->request);
  *slot = (struct leme_control_watch_slot){0};
}

static void close_set(struct leme_control_watch_set *set) {
  if (set->closed)
    return;
  set->closed = true;
  for (size_t i = 0; i < LEME_CONTROL_WATCH_MAX; ++i)
    remove_slot(set, &set->slots[i]);
  set->sink.close(set->sink.context);
}

enum leme_control_code
leme_control_watch_set_create(struct leme_public_budget *account,
                              const struct leme_control_limits *limits,
                              const struct leme_control_watch_sink *sink,
                              struct leme_control_watch_set **out) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (account == NULL || limits == NULL || sink == NULL ||
      sink->publish == NULL || sink->discard == NULL || sink->close == NULL ||
      limits->subscriptions > LEME_CONTROL_WATCH_MAX)
    return LEME_CONTROL_INVALID_ARGUMENT;
  struct leme_control_watch_set *set =
      leme_control_alloc(account, sizeof(*set));
  if (set == NULL)
    return errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                           : LEME_CONTROL_OUT_OF_MEMORY;
  *set = (struct leme_control_watch_set){.account = account,
                                         .limits = *limits,
                                         .sink = *sink,
                                         .next_subscription = 1};
  *out = set;
  return LEME_CONTROL_OK;
}

void leme_control_watch_set_destroy(struct leme_control_watch_set *set) {
  if (set == NULL)
    return;
  for (size_t i = 0; i < LEME_CONTROL_WATCH_MAX; ++i) {
    assert(!set->slots[i].busy);
    remove_slot(set, &set->slots[i]);
  }
  leme_control_free(set);
}

static enum leme_control_code
acknowledgement(struct leme_control_watch_set *set,
                const struct leme_control_request *request,
                struct leme_public_text instance, uint64_t subscription,
                struct leme_control_meter *meter,
                struct leme_control_frame **out) {
  const struct leme_public_text id = leme_control_request_id(request);
  struct leme_public_builder *builder = NULL;
  struct leme_public_value *value = NULL;
  enum leme_control_code code = LEME_CONTROL_OK;
  const struct leme_public_work work = {.context = meter, .step = work_step};
  if (subscription != 0) {
    enum leme_public_status status =
        leme_public_builder_create_budget(set->account, 2048, &builder);
    if (status != LEME_PUBLIC_OK)
      return status == LEME_PUBLIC_OOM ? LEME_CONTROL_OUT_OF_MEMORY
                                       : LEME_CONTROL_RESOURCE_LIMIT;
    leme_public_builder_set_work(builder, &work);
    char token[32] = {0};
    const int length =
        snprintf(token, sizeof(token), "sub:%" PRIu64, subscription);
    if (length < 0 || (size_t)length >= sizeof(token)) {
      code = LEME_CONTROL_RESOURCE_LIMIT;
      goto cleanup;
    }
    status = leme_public_object(builder, 1, &value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_put_cstr(builder, value,
                                    LEME_PUBLIC_TEXT("subscription"), token);
    if (status == LEME_PUBLIC_OK) {
      const struct leme_public_value *roots[] = {value};
      status = leme_public_builder_seal(builder, roots, 1);
    }
    if (status != LEME_PUBLIC_OK) {
      code = status == LEME_PUBLIC_OOM ? LEME_CONTROL_OUT_OF_MEMORY
                                       : LEME_CONTROL_RESOURCE_LIMIT;
      goto cleanup;
    }
  }
  code = leme_control_reply_create_value_metered(
      set->account, set->limits.response_bytes, id.data, id.length,
      instance.data, instance.length, NULL, 0, value, meter, out);
  if (code == LEME_CONTROL_OK)
    leme_control_frame_set_metadata(*out, LEME_CONTROL_FRAME_OTHER, false,
                                    id.data, id.length);
cleanup:
  leme_public_builder_destroy(builder);
  return code;
}

static enum leme_control_code
unwatch(struct leme_control_watch_set *set,
        struct leme_control_context *context,
        const struct leme_control_request *request,
        struct leme_control_error *error) {
  const struct leme_public_text subscription =
      leme_control_request_subscription(request);
  for (size_t i = 0; i < set->limits.subscriptions; ++i) {
    struct leme_control_watch_slot *slot = &set->slots[i];
    if (slot->state == LEME_WATCH_REMOVED)
      continue;
    char token[32] = {0};
    const int length =
        snprintf(token, sizeof(token), "sub:%" PRIu64, slot->subscription);
    if (length < 0 || (size_t)length >= sizeof(token)) {
      close_set(set);
      return fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                  LEME_PUBLIC_TEXT("subscription formatting failed"));
    }
    if (subscription.length != (size_t)length ||
        memcmp(subscription.data, token, subscription.length) != 0)
      continue;
    const struct leme_control_watch_disposal disposal = {
        .subscription = slot->subscription,
        .reason = LEME_WATCH_DISCARD_CANCEL};
    remove_slot(set, slot);
    if (!set->sink.discard(set->sink.context, disposal)) {
      close_set(set);
      return fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                  LEME_PUBLIC_TEXT("watch disposal failed"));
    }
    break;
  }
  struct leme_control_frame *frame = NULL;
  struct leme_control_meter meter = meter_for(set, context);
  enum leme_control_code code = acknowledgement(
      set, request,
      leme_public_model_instance(leme_control_context_model(context)), 0,
      &meter, &frame);
  if (code == LEME_CONTROL_OK)
    code = set->sink.publish(set->sink.context, &frame, 1);
  leme_control_frame_destroy(frame);
  if (code != LEME_CONTROL_OK) {
    close_set(set);
    return fail(error, code,
                LEME_PUBLIC_TEXT("unwatch acknowledgement failed"));
  }
  return LEME_CONTROL_OK;
}

enum leme_control_code leme_control_watch_request(
    struct leme_control_watch_set *set, struct leme_control_context *context,
    struct leme_control_request *request, struct leme_control_error *error) {
  struct leme_control_error local_error = {0};
  if (error == NULL)
    error = &local_error;
  *error = (struct leme_control_error){0};
  if (set == NULL || context == NULL || request == NULL || set->closed ||
      leme_control_context_account(context) != set->account)
    return fail(error, LEME_CONTROL_INVALID_ARGUMENT,
                LEME_PUBLIC_TEXT("invalid watch context"));
  const enum leme_control_request_op operation =
      leme_control_request_operation(request);
  if (operation == LEME_CONTROL_UNWATCH)
    return unwatch(set, context, request, error);
  if (operation != LEME_CONTROL_WATCH)
    return fail(error, LEME_CONTROL_INVALID_REQUEST,
                LEME_PUBLIC_TEXT("expected watch operation"));
  if (set->count >= set->limits.subscriptions ||
      set->next_subscription == UINT64_MAX)
    return fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("subscription limit reached"));
  struct leme_control_watch_slot *slot = NULL;
  for (size_t i = 0; i < set->limits.subscriptions; ++i) {
    if (set->slots[i].state == LEME_WATCH_REMOVED && !set->slots[i].busy) {
      slot = &set->slots[i];
      break;
    }
  }
  if (slot == NULL)
    return fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("no subscription slot"));

  *slot =
      (struct leme_control_watch_slot){.request = request,
                                       .subscription = set->next_subscription++,
                                       .state = LEME_WATCH_ARMING,
                                       .busy = true,
                                       .sensitive = true,
                                       .dirty_epoch = set->dirty_epoch};
  ++set->count;
  leme_control_request_ref(request);
  const uint64_t epoch = set->dirty_epoch;
  const uint64_t privacy = set->privacy_epoch;
  struct leme_control_meter meter = meter_for(set, context);
  const struct leme_public_work work = {.context = &meter,
                                        .step = capture_step};
  struct leme_public_snapshot *snapshot = NULL;
  struct leme_control_evaluation *candidate = NULL;
  struct leme_control_frame *frames[2] = {0};
  enum leme_control_code code = leme_control_compile_work(
      context, request, &meter, &slot->program, error);
  if (code != LEME_CONTROL_OK)
    goto cleanup;
  slot->roots = leme_control_program_roots(slot->program);
  slot->sensitive = (slot->roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0;
  if (slot->roots != 0) {
    const enum leme_public_status status = leme_public_model_capture_work(
        leme_control_context_model(context),
        leme_control_context_source(context), slot->roots, &work, &snapshot);
    if (status != LEME_PUBLIC_OK) {
      code = fail(error,
                  status == LEME_PUBLIC_LOCKED ? LEME_CONTROL_SESSION_LOCKED
                  : status == LEME_PUBLIC_OOM  ? LEME_CONTROL_OUT_OF_MEMORY
                                               : LEME_CONTROL_RESOURCE_LIMIT,
                  LEME_PUBLIC_TEXT("watch capture failed"));
      goto cleanup;
    }
  }
  if (slot->sensitive && privacy != set->privacy_epoch) {
    code = fail(error, LEME_CONTROL_SESSION_LOCKED,
                LEME_PUBLIC_TEXT("watch interrupted by session lock"));
    goto cleanup;
  }
  code = leme_control_evaluate_work(context, slot->program, snapshot, &meter,
                                    &candidate, error);
  if (code != LEME_CONTROL_OK)
    goto cleanup;
  const struct leme_public_text instance =
      leme_public_model_instance(leme_control_context_model(context));
  code = acknowledgement(set, request, instance, slot->subscription, &meter,
                         &frames[0]);
  if (code != LEME_CONTROL_OK)
    goto cleanup;
  const struct leme_control_watch_event event = {
      .kind = LEME_WATCH_SNAPSHOT,
      .subscription = slot->subscription,
      .sequence = 1,
      .instance = instance,
      .revision = leme_public_snapshot_revision(snapshot),
      .value = leme_control_evaluation_value(candidate),
      .sensitive = slot->sensitive};
  code = leme_control_watch_event_create(
      set->account, set->limits.response_bytes, &event, &meter, &frames[1]);
  if (code != LEME_CONTROL_OK)
    goto cleanup;
  if (set->closed || slot->state != LEME_WATCH_ARMING ||
      (slot->sensitive && privacy != set->privacy_epoch) ||
      leme_control_charge(&meter, 0) != LEME_CONTROL_OK) {
    code = fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("watch publication interrupted"));
    goto cleanup;
  }
  code = set->sink.publish(set->sink.context, frames, 2);
  if (code != LEME_CONTROL_OK) {
    close_set(set);
    goto cleanup;
  }
  if (set->closed || slot->state != LEME_WATCH_ARMING ||
      (slot->sensitive && privacy != set->privacy_epoch)) {
    close_set(set);
    code = fail(error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("watch publication interrupted"));
    goto cleanup;
  }
  slot->baseline = candidate;
  candidate = NULL;
  slot->sequence = 1;
  slot->state = LEME_WATCH_ACTIVE;
  slot->dirty = set->dirty_epoch != epoch;

cleanup:
  if (code != LEME_CONTROL_OK && slot->sensitive &&
      privacy != set->privacy_epoch)
    code = fail(error, LEME_CONTROL_SESSION_LOCKED,
                LEME_PUBLIC_TEXT("watch interrupted by session lock"));
  leme_control_frame_destroy(frames[0]);
  const bool sensitive_error = slot->sensitive && slot->program != NULL &&
                               code != LEME_CONTROL_SESSION_LOCKED;
  leme_control_frame_destroy(frames[1]);
  leme_control_evaluation_destroy(candidate);
  leme_public_snapshot_unref(snapshot);
  slot->busy = false;
  if (code != LEME_CONTROL_OK || slot->state == LEME_WATCH_REMOVED)
    remove_slot(set, slot);
  if (code != LEME_CONTROL_OK && error->code == LEME_CONTROL_OK)
    (void)fail(error, code, LEME_PUBLIC_TEXT("watch registration failed"));
  error->sensitive = code != LEME_CONTROL_OK && sensitive_error;
  return code;
}

void leme_control_watch_notify(struct leme_control_watch_set *set,
                               enum leme_public_change change) {
  if (set == NULL || set->closed)
    return;
  if (change == LEME_PUBLIC_DISABLED || set->dirty_epoch == UINT64_MAX ||
      (change != LEME_PUBLIC_CHANGED && set->privacy_epoch == UINT64_MAX)) {
    close_set(set);
    return;
  }
  ++set->dirty_epoch;
  if (change != LEME_PUBLIC_CHANGED)
    ++set->privacy_epoch;
  if (change == LEME_PUBLIC_LOCKED_CHANGED &&
      !set->sink.discard(set->sink.context,
                         (struct leme_control_watch_disposal){
                             .reason = LEME_WATCH_DISCARD_LOCK})) {
    close_set(set);
    return;
  }
  for (size_t i = 0; i < set->limits.subscriptions; ++i) {
    struct leme_control_watch_slot *slot = &set->slots[i];
    if (slot->state == LEME_WATCH_REMOVED)
      continue;
    slot->dirty = true;
    slot->dirty_epoch = set->dirty_epoch;
    if (!slot->sensitive)
      continue;
    if (change == LEME_PUBLIC_LOCKED_CHANGED) {
      if (!set->sink.discard(set->sink.context,
                             (struct leme_control_watch_disposal){
                                 .subscription = slot->subscription,
                                 .reason = LEME_WATCH_DISCARD_LOCK})) {
        close_set(set);
        return;
      }
      leme_control_evaluation_destroy(slot->baseline);
      slot->baseline = NULL;
      if (slot->state != LEME_WATCH_ARMING) {
        slot->state = LEME_WATCH_SUSPENDED_LOCKED;
        slot->suspension_pending = true;
        slot->reset = true;
      }
    } else if (change == LEME_PUBLIC_UNLOCKED_CHANGED &&
               slot->state == LEME_WATCH_SUSPENDED_LOCKED) {
      slot->state = LEME_WATCH_ACTIVE;
    }
  }
}

bool leme_control_watch_has_work(const struct leme_control_watch_set *set) {
  if (set == NULL || set->closed)
    return false;
  for (size_t i = 0; i < set->limits.subscriptions; ++i) {
    if (!set->slots[i].busy &&
        (set->slots[i].suspension_pending ||
         (set->slots[i].state == LEME_WATCH_ACTIVE && set->slots[i].dirty)))
      return true;
  }
  return false;
}

void leme_control_watch_step(struct leme_control_watch_set *set,
                             struct leme_control_context *context) {
  if (set == NULL || context == NULL || set->closed ||
      leme_control_context_account(context) != set->account)
    return;
  struct leme_control_watch_slot *slot = NULL;
  for (size_t offset = 0; offset < set->limits.subscriptions; ++offset) {
    const size_t index = (set->cursor + offset) % set->limits.subscriptions;
    struct leme_control_watch_slot *candidate_slot = &set->slots[index];
    if (!candidate_slot->busy && (candidate_slot->suspension_pending ||
                                  (candidate_slot->state == LEME_WATCH_ACTIVE &&
                                   candidate_slot->dirty))) {
      slot = candidate_slot;
      set->cursor = (index + 1) % set->limits.subscriptions;
      break;
    }
  }
  if (slot == NULL)
    return;
  slot->busy = true;
  const uint64_t epoch = set->dirty_epoch;
  const uint64_t privacy = set->privacy_epoch;
  struct leme_control_evaluation *previous = slot->baseline;
  slot->baseline = NULL;
  struct leme_control_evaluation *candidate = NULL;
  struct leme_public_snapshot *snapshot = NULL;
  struct leme_control_frame *frame = NULL;
  struct leme_control_error error = {0};
  struct leme_control_meter meter = meter_for(set, context);
  const struct leme_public_work work = {.context = &meter, .step = work_step};
  const struct leme_public_work capture_work = {.context = &meter,
                                                .step = capture_step};
  enum leme_control_code code = LEME_CONTROL_OK;
  if (slot->suspension_pending) {
    if (slot->sequence == UINT64_MAX) {
      close_set(set);
      goto cleanup;
    }
    const bool exhausted = slot->sequence == UINT64_MAX - 1;
    if (exhausted)
      error =
          (struct leme_control_error){.code = LEME_CONTROL_RESOURCE_LIMIT,
                                      .phase = LEME_CONTROL_EVALUATE,
                                      .message = "watch sequence exhausted"};
    const struct leme_control_watch_event notice = {
        .kind = exhausted ? LEME_WATCH_ERROR : LEME_WATCH_SUSPENDED,
        .error = exhausted ? &error : NULL,
        .subscription = slot->subscription,
        .sequence = slot->sequence + 1,
        .instance =
            leme_public_model_instance(leme_control_context_model(context))};
    code = leme_control_watch_event_create(
        set->account, set->limits.response_bytes, &notice, &meter, &frame);
    if (code == LEME_CONTROL_OK)
      code = set->sink.publish(set->sink.context, &frame, 1);
    if (code != LEME_CONTROL_OK || set->closed ||
        slot->state == LEME_WATCH_REMOVED || privacy != set->privacy_epoch) {
      close_set(set);
    } else {
      ++slot->sequence;
      slot->suspension_pending = false;
      if (exhausted)
        remove_slot(set, slot);
    }
    goto cleanup;
  }
  if (slot->roots != 0) {
    const enum leme_public_status status =
        leme_public_model_capture_work(leme_control_context_model(context),
                                       leme_control_context_source(context),
                                       slot->roots, &capture_work, &snapshot);
    if (status == LEME_PUBLIC_LOCKED && !slot->sensitive && !set->closed &&
        slot->state == LEME_WATCH_ACTIVE) {
      slot->baseline = previous;
      previous = NULL;
      slot->dirty = true;
      goto cleanup;
    }
    if (status != LEME_PUBLIC_OK) {
      code = fail(&error,
                  status == LEME_PUBLIC_LOCKED ? LEME_CONTROL_SESSION_LOCKED
                  : status == LEME_PUBLIC_OOM  ? LEME_CONTROL_OUT_OF_MEMORY
                                               : LEME_CONTROL_RESOURCE_LIMIT,
                  LEME_PUBLIC_TEXT("watch capture failed"));
      goto terminal;
    }
  }
  code = leme_control_evaluate_work(context, slot->program, snapshot, &meter,
                                    &candidate, &error);
  if (code != LEME_CONTROL_OK)
    goto terminal;
  if (set->closed || slot->state != LEME_WATCH_ACTIVE ||
      (slot->sensitive && privacy != set->privacy_epoch))
    goto cleanup;
  bool same = false;
  const enum leme_public_status equal_status =
      slot->reset
          ? LEME_PUBLIC_OK
          : leme_public_equal_checked(leme_control_evaluation_value(previous),
                                      leme_control_evaluation_value(candidate),
                                      &work, &same);
  if (equal_status != LEME_PUBLIC_OK) {
    code = fail(&error,
                equal_status == LEME_PUBLIC_OOM ? LEME_CONTROL_OUT_OF_MEMORY
                                                : LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("watch comparison failed"));
    error.phase = LEME_CONTROL_EVALUATE;
    goto terminal;
  }
  if (same) {
    slot->baseline = previous;
    previous = NULL;
    slot->dirty = set->dirty_epoch != epoch;
    goto cleanup;
  }
  if (slot->sequence >= UINT64_MAX - 1) {
    code = fail(&error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("watch sequence exhausted"));
    error.phase = LEME_CONTROL_EVALUATE;
    goto terminal;
  }
  const struct leme_control_watch_event event = {
      .kind = slot->reset ? LEME_WATCH_RESET : LEME_WATCH_CHANGE,
      .subscription = slot->subscription,
      .sequence = slot->sequence + 1,
      .instance =
          leme_public_model_instance(leme_control_context_model(context)),
      .revision = leme_public_snapshot_revision(snapshot),
      .value = leme_control_evaluation_value(candidate),
      .sensitive = slot->sensitive};
  code = leme_control_watch_event_create(
      set->account, set->limits.response_bytes, &event, &meter, &frame);
  if (code != LEME_CONTROL_OK)
    goto terminal;
  if (set->closed || slot->state != LEME_WATCH_ACTIVE ||
      (slot->sensitive && privacy != set->privacy_epoch))
    goto cleanup;
  if (leme_control_charge(&meter, 0) != LEME_CONTROL_OK) {
    code = fail(&error, LEME_CONTROL_RESOURCE_LIMIT,
                LEME_PUBLIC_TEXT("watch budget exceeded"));
    error.phase = LEME_CONTROL_EVALUATE;
    goto terminal;
  }
  code = set->sink.publish(set->sink.context, &frame, 1);
  if (code != LEME_CONTROL_OK || set->closed ||
      slot->state != LEME_WATCH_ACTIVE ||
      (slot->sensitive && privacy != set->privacy_epoch)) {
    close_set(set);
    goto cleanup;
  }
  slot->baseline = candidate;
  candidate = NULL;
  ++slot->sequence;
  slot->reset = false;
  slot->dirty = set->dirty_epoch != epoch;
  goto cleanup;

terminal:
  leme_control_frame_destroy(frame);
  frame = NULL;
  if (set->closed || slot->state != LEME_WATCH_ACTIVE ||
      (slot->sensitive && privacy != set->privacy_epoch))
    goto cleanup;
  if (slot->sequence == UINT64_MAX) {
    close_set(set);
    goto cleanup;
  }
  if (error.code == LEME_CONTROL_OK)
    (void)fail(&error, code, LEME_PUBLIC_TEXT("watch evaluation failed"));
  const struct leme_control_watch_event failure = {
      .kind = LEME_WATCH_ERROR,
      .subscription = slot->subscription,
      .sequence = slot->sequence + 1,
      .instance =
          leme_public_model_instance(leme_control_context_model(context)),
      .error = &error,
      .sensitive = slot->sensitive};
  code = leme_control_watch_event_create(
      set->account, set->limits.response_bytes, &failure, NULL, &frame);
  if (code == LEME_CONTROL_OK)
    code = set->sink.publish(set->sink.context, &frame, 1);
  if (code != LEME_CONTROL_OK || set->closed ||
      slot->state != LEME_WATCH_ACTIVE ||
      (slot->sensitive && privacy != set->privacy_epoch))
    close_set(set);
  remove_slot(set, slot);

cleanup:
  leme_control_frame_destroy(frame);
  leme_control_evaluation_destroy(previous);
  leme_control_evaluation_destroy(candidate);
  leme_public_snapshot_unref(snapshot);
  slot->busy = false;
  if (slot->state == LEME_WATCH_REMOVED)
    remove_slot(set, slot);
}
