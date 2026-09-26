#include "timao/client-internal.h"
#include "control/memory.h"
#include "control/registry.h"
#include "ipc/json.h"
#include "public/json.h"
#include "public/model.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const struct leme_public_value *field(const struct leme_public_value *v,
                                             const char *name) {
  return leme_public_get(v, (struct leme_public_text){name, strlen(name)});
}
static bool text_equal(struct leme_public_text a, struct leme_public_text b) {
  return a.length == b.length &&
         (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}
static bool is_text(const struct leme_public_value *v, const char *name) {
  struct leme_public_text text = {0};
  return leme_public_as_text(v, &text) == LEME_PUBLIC_OK &&
         text_equal(text, (struct leme_public_text){name, strlen(name)});
}
static struct timao_client_watch_slot *find(struct timao_client *client,
                                            uint64_t id) {
  for (size_t i = 0; i < 32; ++i)
    if (client->watches[i].id == id && id != 0)
      return &client->watches[i];
  return NULL;
}
bool timao_client_watch_canceled(struct timao_client *client, uint64_t watch) {
  const struct timao_client_watch_slot *w = find(client, watch);
  return w == NULL || w->canceled;
}

static void remove_watch(struct timao_client *client,
                         struct timao_client_watch_slot *w) {
  leme_control_document_destroy(w->descriptor);
  timao_client_message_destroy(w->terminal);
  *w = (struct timao_client_watch_slot){0};
  --client->watch_count;
}
void timao_client_watch_terminate(struct timao_client *client,
                                  struct timao_client_watch_slot *w,
                                  enum timao_client_cause cause) {
  w->terminal->failure.cause = cause;
  timao_client_enqueue(client, w->terminal);
  w->terminal = NULL;
  remove_watch(client, w);
}

void timao_client_watches_clear(struct timao_client *client) {
  for (size_t i = 0; i < 32; ++i)
    if (client->watches[i].id != 0)
      remove_watch(client, &client->watches[i]);
}

static void purge(struct timao_client *client, uint64_t watch,
                  bool values_only) {
  struct timao_client_message *previous = NULL;
  struct timao_client_message *message = client->head;
  while (message != NULL) {
    struct timao_client_message *next = message->next;
    const bool match =
        message->watch != 0 && (watch == 0 || message->watch == watch);
    const bool value =
        message->kind == TIMAO_CLIENT_EVENT &&
        field(timao_client_message_value(message), "value") != NULL;
    if (match && (!values_only || value)) {
      if (previous != NULL)
        previous->next = next;
      else
        client->head = next;
      if (client->tail == message)
        client->tail = previous;
      client->queued_bytes -= message->queued_bytes;
      timao_client_message_destroy(message);
    } else {
      previous = message;
    }
    message = next;
  }
}

static bool sensitive(const struct leme_public_value *expression, size_t depth,
                      size_t *remaining) {
  if (depth > 64 || *remaining == 0 ||
      leme_public_kind(expression) != LEME_PUBLIC_OBJECT)
    return true;
  --*remaining;
  if (leme_public_length(expression) == 1 &&
      field(expression, "literal") != NULL)
    return false;
  const struct leme_public_value *path = field(expression, "field");
  if (leme_public_length(expression) == 1 && path != NULL)
    return leme_public_kind(path) != LEME_PUBLIC_ARRAY;
  const struct leme_public_value *args = field(expression, "args");
  struct leme_public_text name = {0};
  if (leme_public_length(expression) != 2 ||
      leme_public_kind(args) != LEME_PUBLIC_ARRAY ||
      leme_public_as_text(field(expression, "call"), &name) != LEME_PUBLIC_OK)
    return true;
  const struct leme_control_operator *op = leme_control_operator_find(name);
  if (op == NULL || op->effect != LEME_CONTROL_EFFECT_PURE ||
      (leme_control_operator_roots(op, NULL, 0) & ~LEME_PUBLIC_SAFE_ROOTS) != 0)
    return true;
  const size_t count = leme_public_length(args);
  if (count < op->min_args || count > op->max_args)
    return true;
  for (size_t i = 0; i < count; ++i)
    if (sensitive(leme_public_at(args, i), depth + 1, remaining))
      return true;
  return false;
}

struct decode_bounds {
  size_t bytes, arena;
};

static enum leme_public_status decode_json(struct timao_client *client,
                                           struct leme_json *json,
                                           struct decode_bounds bounds,
                                           struct leme_control_document **out) {
  if (bounds.arena == 0)
    return LEME_PUBLIC_LIMIT;
  if (json->failed)
    return json->error == LEME_JSON_ERROR_OOM ? LEME_PUBLIC_OOM
                                              : LEME_PUBLIC_LIMIT;
  struct leme_control_limits limits = leme_control_limits_default();
  limits.request_bytes = bounds.bytes;
  limits.retained_bytes = bounds.arena;
  struct leme_control_meter meter = {.remaining = 16777216};
  const enum leme_control_code code = leme_control_decode(
      client->account, (struct leme_public_text){json->data, json->length},
      &limits, &meter, out, NULL);
  return code == LEME_CONTROL_OK               ? LEME_PUBLIC_OK
         : code == LEME_CONTROL_OUT_OF_MEMORY  ? LEME_PUBLIC_OOM
         : code == LEME_CONTROL_RESOURCE_LIMIT ? LEME_PUBLIC_LIMIT
                                               : LEME_PUBLIC_INVALID;
}

static enum leme_public_status
copy_descriptor(struct timao_client *client,
                const struct leme_public_value *expression,
                struct leme_control_document **out) {
  struct leme_json json = {0};
  leme_json_init_budget(&json, client->account, client->remote.request_bytes);
  enum leme_public_status status = leme_public_write_json(expression, &json);
  if (status == LEME_PUBLIC_OK)
    status = decode_json(
        client, &json,
        (struct decode_bounds){.bytes = client->remote.request_bytes,
                               .arena = client->limits.queued_bytes},
        out);
  leme_json_finish(&json);
  return status;
}

static enum leme_public_status
normalized(struct timao_client *client, const struct timao_client_watch_slot *w,
           const struct leme_public_value *event,
           struct leme_control_document **out) {
  struct leme_json json = {0};
  leme_json_init_budget(&json, client->account, client->limits.queued_bytes);
  leme_json_object_begin(&json);
  enum leme_public_status status = LEME_PUBLIC_OK;
  const bool recovered =
      w->reset_reason != NULL && field(event, "value") != NULL;
  for (size_t i = 0; i < leme_public_length(event); ++i) {
    const struct leme_public_text key = leme_public_key_at(event, i);
    if (text_equal(key, LEME_PUBLIC_TEXT("watch")) ||
        text_equal(key, LEME_PUBLIC_TEXT("generation")) ||
        (recovered && (text_equal(key, LEME_PUBLIC_TEXT("event")) ||
                       text_equal(key, LEME_PUBLIC_TEXT("reason")))))
      continue;
    leme_json_key_n(&json, key.data, key.length);
    status = leme_public_write_json(leme_public_member_at(event, i), &json);
    if (status != LEME_PUBLIC_OK)
      break;
  }
  char id[32] = {0};
  const int n = snprintf(id, sizeof(id), "watch:%" PRIu64, w->id);
  if (n < 0 || (size_t)n >= sizeof(id))
    status = LEME_PUBLIC_LIMIT;
  if (status == LEME_PUBLIC_OK) {
    if (recovered) {
      leme_json_key(&json, "event");
      leme_json_string(&json, "reset");
      leme_json_key(&json, "reason");
      leme_json_string(&json, w->reset_reason);
    }
    leme_json_key(&json, "watch");
    leme_json_string(&json, id);
    leme_json_key(&json, "generation");
    leme_json_integer(&json, (int64_t)w->generation);
    leme_json_object_end(&json);
    const size_t metadata = sizeof(struct timao_client_message) +
                            leme_control_allocation_overhead();
    status =
        decode_json(client, &json,
                    (struct decode_bounds){
                        .bytes = client->limits.queued_bytes,
                        .arena = timao_client_delivery_arena(client, metadata)},
                    out);
  }
  leme_json_finish(&json);
  return status;
}

enum leme_public_status
timao_client_watch(struct timao_client *client,
                   const struct leme_public_value *expression, uint64_t *watch,
                   struct timao_client_ticket *ticket) {
  if (watch == NULL || ticket == NULL)
    return LEME_PUBLIC_INVALID;
  *watch = 0;
  *ticket = (struct timao_client_ticket){0};
  if (client == NULL || expression == NULL)
    return LEME_PUBLIC_INVALID;
  timao_client_tick(client);
  if (!timao_client_ready(client))
    return LEME_PUBLIC_UNAVAILABLE;
  if (client->next_watch == UINT64_MAX ||
      client->watch_count >= client->limits.watches ||
      (client->watch_capability &&
       client->watch_count >= client->remote.subscriptions))
    return LEME_PUBLIC_LIMIT;
  struct timao_client_watch_slot *w = NULL;
  for (size_t i = 0; i < 32; ++i)
    if (client->watches[i].id == 0) {
      w = &client->watches[i];
      break;
    }
  if (w == NULL)
    return LEME_PUBLIC_LIMIT;
  struct timao_client_message *terminal =
      leme_control_alloc(client->account, sizeof(*terminal));
  if (terminal == NULL)
    return timao_client_allocation_status();
  const uint64_t id = client->next_watch + 1;
  *terminal = (struct timao_client_message){
      .kind = TIMAO_CLIENT_FAILURE,
      .watch = id,
      .failure = {.operation = TIMAO_CLIENT_WATCH, .effects_known_none = true}};
  struct leme_control_document *descriptor = NULL;
  enum leme_public_status status = LEME_PUBLIC_OK;
  if (client->watch_capability) {
    status = copy_descriptor(client, expression, &descriptor);
    if (status == LEME_PUBLIC_OK)
      status = timao_client_admit(client, TIMAO_CLIENT_WATCH,
                                  leme_control_document_value(descriptor),
                                  (struct leme_public_text){0}, id, ticket);
    if (status != LEME_PUBLIC_OK) {
      leme_control_document_destroy(descriptor);
      timao_client_message_destroy(terminal);
      return status;
    }
  }
  ++client->next_watch;
  *watch = id;
  if (!client->watch_capability) {
    terminal->failure.cause = TIMAO_CLIENT_UNSUPPORTED;
    timao_client_enqueue(client, terminal);
    return LEME_PUBLIC_OK;
  }
  terminal->ticket = *ticket;
  size_t remaining = 4096;
  *w = (struct timao_client_watch_slot){
      .id = id,
      .descriptor = descriptor,
      .terminal = terminal,
      .pending_registration = true,
      .sensitive =
          sensitive(leme_control_document_value(descriptor), 1, &remaining)};
  ++client->watch_count;
  return LEME_PUBLIC_OK;
}

void timao_client_watch_interest(struct timao_client *client, uint64_t watch,
                                 bool live) {
  if (client == NULL)
    return;
  struct timao_client_watch_slot *w = find(client, watch);
  if (w != NULL && !w->canceled)
    w->interest = live;
}

void timao_client_watches_pump(struct timao_client *client) {
  if (client->state != TIMAO_CLIENT_READY)
    return;
  for (size_t i = 0; i < 32; ++i) {
    struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id == 0 || !w->canceled || w->pending_registration ||
        w->pending_unwatch)
      continue;
    if (client->pending_count >= client->remote.outstanding)
      return;
    if (w->subscription_length == 0) {
      remove_watch(client, w);
      continue;
    }
    struct timao_client_ticket ticket = {0};
    const enum leme_public_status status = timao_client_admit(
        client, TIMAO_CLIENT_UNWATCH, NULL,
        (struct leme_public_text){w->subscription, w->subscription_length},
        w->id, &ticket);
    if (status != LEME_PUBLIC_OK) {
      timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
      return;
    }
    w->pending_unwatch = true;
  }
}

void timao_client_cancel(struct timao_client *client, uint64_t watch) {
  if (client == NULL)
    return;
  struct timao_client_watch_slot *w = find(client, watch);
  if (w == NULL || w->canceled)
    return;
  w->canceled = true;
  w->interest = false;
  purge(client, watch, false);
  leme_control_document_destroy(w->descriptor);
  w->descriptor = NULL;
  if (w->lost && !w->pending_registration && w->subscription_length == 0) {
    remove_watch(client, w);
    return;
  }
  timao_client_watches_pump(client);
}

enum leme_public_status
timao_client_watch_reply(struct timao_client *client,
                         struct timao_client_pending *pending,
                         struct leme_control_document **doc, bool ok) {
  struct timao_client_watch_slot *w = find(client, pending->completion->watch);
  if (w == NULL)
    return LEME_PUBLIC_INVALID;
  const struct leme_public_value *root = leme_control_document_value(*doc);
  if (pending->completion->failure.operation == TIMAO_CLIENT_UNWATCH) {
    if (!ok || leme_public_kind(field(root, "value")) != LEME_PUBLIC_NULL)
      return LEME_PUBLIC_INVALID;
    remove_watch(client, w);
    return LEME_PUBLIC_OK;
  }
  w->pending_registration = false;
  if (!ok && w->acknowledged && w->sensitive && !w->canceled &&
      is_text(field(field(root, "error"), "code"), "session_locked")) {
    client->recovery_locked = true;
    w->lost = true;
    return LEME_PUBLIC_OK;
  }
  if (!ok) {
    if (!w->canceled) {
      pending->completion->kind = TIMAO_CLIENT_FAILURE;
      pending->completion->failure.cause = TIMAO_CLIENT_UNSUPPORTED;
      pending->completion->failure.effects_known_none = true;
      pending->completion->document = *doc;
      if (!timao_client_deliver(client, pending->completion)) {
        pending->completion->document = NULL;
        return LEME_PUBLIC_LIMIT;
      }
      *doc = NULL;
      pending->completion = NULL;
    }
    remove_watch(client, w);
    return LEME_PUBLIC_OK;
  }
  struct leme_public_text subscription = {0};
  if (leme_public_as_text(field(field(root, "value"), "subscription"),
                          &subscription) != LEME_PUBLIC_OK ||
      subscription.length == 0 || subscription.length > 128 ||
      memchr(subscription.data, '\0', subscription.length) != NULL ||
      leme_public_kind(field(root, "revision")) != LEME_PUBLIC_NULL)
    return LEME_PUBLIC_INVALID;
  if (client->helper_length != 0 &&
      text_equal(subscription,
                 (struct leme_public_text){client->helper_subscription,
                                           client->helper_length}))
    return LEME_PUBLIC_INVALID;
  for (size_t i = 0; i < 32; ++i) {
    const struct timao_client_watch_slot *other = &client->watches[i];
    if (other != w && other->id != 0 &&
        text_equal(subscription,
                   (struct leme_public_text){other->subscription,
                                             other->subscription_length}))
      return LEME_PUBLIC_INVALID;
  }
  if (client->instance.length == SIZE_MAX ||
      pending->completion->failure.request_id.length >=
          sizeof(w->terminal->request_id))
    return LEME_PUBLIC_LIMIT;
  char *instance_copy =
      leme_control_alloc(client->account, client->instance.length + 1);
  if (instance_copy == NULL)
    return timao_client_allocation_status();
  memcpy(instance_copy, client->instance.data, client->instance.length);
  instance_copy[client->instance.length] = '\0';
  const bool restored = w->acknowledged;
  if (restored)
    w->reset_reason =
        text_equal(w->terminal->failure.instance, client->instance)
            ? "reconnect"
            : "instance_changed";
  leme_control_free(w->terminal->instance_copy);
  w->terminal->instance_copy = instance_copy;
  w->terminal->failure.instance =
      (struct leme_public_text){instance_copy, client->instance.length};
  memcpy(w->terminal->request_id, pending->completion->request_id,
         sizeof(w->terminal->request_id));
  w->terminal->failure.request_id = (struct leme_public_text){
      w->terminal->request_id, pending->completion->failure.request_id.length};
  w->terminal->ticket = pending->completion->ticket;
  memcpy(w->subscription, subscription.data, subscription.length);
  w->subscription[subscription.length] = '\0';
  w->subscription_length = subscription.length;
  w->acknowledged = true;
  w->sequence = 0;
  if (w->generation >= (uint64_t)LEME_PUBLIC_SAFE_INTEGER) {
    w->canceled = true;
    w->interest = false;
    purge(client, w->id, false);
    leme_control_document_destroy(w->descriptor);
    w->descriptor = NULL;
    pending->completion->kind = TIMAO_CLIENT_FAILURE;
    pending->completion->failure.cause = TIMAO_CLIENT_RESOURCE;
    pending->completion->failure.effects_known_none = true;
    timao_client_enqueue(client, pending->completion);
    pending->completion = NULL;
    return LEME_PUBLIC_OK;
  }
  ++w->generation;
  w->suspended = false;
  if (!w->canceled && !restored) {
    pending->completion->kind = TIMAO_CLIENT_REGISTERED;
    pending->completion->document = *doc;
    if (!timao_client_deliver(client, pending->completion)) {
      pending->completion->document = NULL;
      return LEME_PUBLIC_LIMIT;
    }
    *doc = NULL;
    pending->completion = NULL;
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status
timao_client_watch_event(struct timao_client *client,
                         const struct leme_public_value *event) {
  struct leme_public_text subscription = {0};
  struct leme_public_text instance = {0};
  uint64_t sequence = 0;
  if (leme_public_as_text(field(event, "subscription"), &subscription) !=
          LEME_PUBLIC_OK ||
      leme_public_as_text(field(event, "instance"), &instance) !=
          LEME_PUBLIC_OK ||
      !text_equal(instance, client->instance) ||
      !timao_client_decimal(field(event, "sequence"), &sequence))
    return LEME_PUBLIC_INVALID;
  if (client->helper_length != 0 &&
      text_equal(subscription,
                 (struct leme_public_text){client->helper_subscription,
                                           client->helper_length}))
    return timao_client_helper_event(client, event);
  struct timao_client_watch_slot *w = NULL;
  for (size_t i = 0; i < 32; ++i)
    if (client->watches[i].id != 0 &&
        text_equal(subscription, (struct leme_public_text){
                                     client->watches[i].subscription,
                                     client->watches[i].subscription_length})) {
      w = &client->watches[i];
      break;
    }
  if (w == NULL || w->pending_registration)
    return LEME_PUBLIC_INVALID;
  if (w->canceled)
    return LEME_PUBLIC_OK;
  if (sequence <= w->sequence ||
      (!w->sensitive &&
       leme_public_kind(field(event, "revision")) != LEME_PUBLIC_NULL))
    return LEME_PUBLIC_INVALID;
  const bool suspended = is_text(field(event, "event"), "suspended");
  const bool terminal = is_text(field(event, "event"), "error");
  const bool reset = is_text(field(event, "event"), "reset");
  if (w->suspended && !suspended && !terminal && !reset)
    return LEME_PUBLIC_INVALID;
  if (sequence == UINT64_MAX && !terminal)
    return LEME_PUBLIC_INVALID;
  if (suspended)
    purge(client, w->id, true);
  struct leme_control_document *doc = NULL;
  enum leme_public_status status = normalized(client, w, event, &doc);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct timao_client_message *message =
      leme_control_alloc(client->account, sizeof(*message));
  if (message == NULL) {
    status = timao_client_allocation_status();
    leme_control_document_destroy(doc);
    return status;
  }
  *message = (struct timao_client_message){.kind = TIMAO_CLIENT_EVENT,
                                           .watch = w->id,
                                           .document = doc,
                                           .ticket = {.epoch = client->epoch}};
  if (!timao_client_deliver(client, message)) {
    timao_client_message_destroy(message);
    return LEME_PUBLIC_LIMIT;
  }
  w->sequence = sequence;
  w->suspended = suspended;
  if (field(event, "value") != NULL) {
    w->lost = false;
    w->reset_reason = NULL;
  }
  if (terminal)
    remove_watch(client, w);
  return LEME_PUBLIC_OK;
}

void timao_client_watches_lost(struct timao_client *client,
                               enum timao_client_cause cause) {
  purge(client, 0, true);
  for (size_t i = 0; i < 32; ++i) {
    struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id == 0)
      continue;
    if (w->canceled || !w->acknowledged) {
      remove_watch(client, w);
      continue;
    }
    if (!w->interest || cause == TIMAO_CLIENT_RESOURCE ||
        cause == TIMAO_CLIENT_PROTOCOL || cause == TIMAO_CLIENT_PERMISSION ||
        cause == TIMAO_CLIENT_UNSUPPORTED || cause == TIMAO_CLIENT_CANCELLED) {
      w->terminal->failure.cause = cause;
      timao_client_enqueue(client, w->terminal);
      w->terminal = NULL;
      remove_watch(client, w);
      continue;
    }
    w->lost = true;
    w->subscription_length = 0;
    w->subscription[0] = '\0';
    w->pending_registration = false;
    w->pending_unwatch = false;
  }
}
