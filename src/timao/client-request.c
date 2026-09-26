#include "timao/client-internal.h"
#include "control/memory.h"
#include "ipc/json.h"
#include "public/json.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const struct leme_public_value *field(const struct leme_public_value *v,
                                             const char *name) {
  return leme_public_get(v, (struct leme_public_text){name, strlen(name)});
}

static bool equal_text(struct leme_public_text a, struct leme_public_text b) {
  return a.length == b.length &&
         (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

static bool is_text(const struct leme_public_value *v, const char *text) {
  struct leme_public_text actual = {0};
  return leme_public_as_text(v, &actual) == LEME_PUBLIC_OK &&
         equal_text(actual, (struct leme_public_text){text, strlen(text)});
}

static struct timao_client_pending *first_pending(struct timao_client *client,
                                                  bool writable) {
  struct timao_client_pending *first = NULL;
  for (size_t i = 0; i < 16; ++i) {
    struct timao_client_pending *p = &client->pending[i];
    if (p->completion != NULL && (!writable || p->frame != NULL) &&
        (first == NULL ||
         p->completion->ticket.serial < first->completion->ticket.serial))
      first = p;
  }
  return first;
}

static void clear_pending(struct timao_client *client,
                          struct timao_client_pending *p) {
  leme_control_frame_destroy(p->frame);
  timao_client_message_destroy(p->completion);
  *p = (struct timao_client_pending){0};
  --client->pending_count;
}

void timao_client_requests_clear(struct timao_client *client) {
  for (size_t i = 0; i < 16; ++i)
    if (client->pending[i].completion != NULL)
      clear_pending(client, &client->pending[i]);
}

void timao_client_requests_fail(struct timao_client *client,
                                enum timao_client_cause cause) {
  struct timao_client_pending *p = NULL;
  while ((p = first_pending(client, false)) != NULL) {
    struct timao_client_message *message = p->completion;
    if (p->internal ||
        (client->status_ticket.serial != 0 &&
         message->ticket.serial == client->status_ticket.serial) ||
        message->failure.operation == TIMAO_CLIENT_UNWATCH ||
        (message->watch != 0 &&
         timao_client_watch_canceled(client, message->watch))) {
      clear_pending(client, p);
      continue;
    }
    message->kind = TIMAO_CLIENT_FAILURE;
    message->failure.cause = cause;
    message->failure.outcome_unknown =
        message->failure.operation == TIMAO_CLIENT_ACT &&
        (p->offset == p->length ||
         (cause == TIMAO_CLIENT_AMBIGUOUS_IO && p->offered));
    message->failure.effects_known_none = !message->failure.outcome_unknown;
    if (message->failure.operation == TIMAO_CLIENT_HELLO)
      timao_client_transport_notice(client, message);
    else
      timao_client_enqueue(client, message);
    p->completion = NULL;
    clear_pending(client, p);
  }
}

void timao_client_requests_tick(struct timao_client *client, uint64_t now) {
  for (size_t i = 0; i < 16; ++i) {
    if (client->pending[i].completion != NULL &&
        now >= client->pending[i].deadline) {
      timao_client_fail(client, TIMAO_CLIENT_TIMEOUT);
      return;
    }
  }
}

static enum leme_public_status
copy_instance(struct timao_client *client, struct timao_client_message *message,
              struct leme_public_text instance) {
  if (instance.length == 0)
    return LEME_PUBLIC_OK;
  if (instance.length == SIZE_MAX)
    return LEME_PUBLIC_LIMIT;
  char *copy = leme_control_alloc(client->account, instance.length + 1);
  if (copy == NULL)
    return timao_client_allocation_status();
  memcpy(copy, instance.data, instance.length);
  copy[instance.length] = '\0';
  leme_control_free(message->instance_copy);
  message->instance_copy = copy;
  message->failure.instance = (struct leme_public_text){copy, instance.length};
  return LEME_PUBLIC_OK;
}

static enum leme_public_status json_status(const struct leme_json *json) {
  if (!json->failed)
    return LEME_PUBLIC_OK;
  return json->error == LEME_JSON_ERROR_OOM     ? LEME_PUBLIC_OOM
         : json->error == LEME_JSON_ERROR_LIMIT ? LEME_PUBLIC_LIMIT
                                                : LEME_PUBLIC_INVALID;
}

static bool depth_fits(const struct leme_public_value *value,
                       const struct leme_control_limits *limits, size_t depth) {
  if (depth > limits->json_depth)
    return false;
  const enum leme_public_kind kind = leme_public_kind(value);
  if (kind != LEME_PUBLIC_ARRAY && kind != LEME_PUBLIC_OBJECT)
    return true;
  for (size_t i = 0; i < leme_public_length(value); ++i) {
    const struct leme_public_value *child =
        kind == LEME_PUBLIC_ARRAY ? leme_public_at(value, i)
                                  : leme_public_member_at(value, i);
    if (!depth_fits(child, limits, depth + 1))
      return false;
  }
  return true;
}

static bool expression_fits(const struct leme_public_value *expression,
                            const struct leme_control_limits *limits,
                            size_t depth, size_t *remaining) {
  if (depth > limits->expression_depth || *remaining == 0)
    return false;
  --*remaining;
  if (field(expression, "literal") != NULL)
    return true;
  const struct leme_public_value *path = field(expression, "field");
  if (path != NULL && leme_public_length(path) > limits->field_depth)
    return false;
  const struct leme_public_value *args = field(expression, "args");
  for (size_t i = 0; i < leme_public_length(args); ++i)
    if (!expression_fits(leme_public_at(args, i), limits, depth + 1, remaining))
      return false;
  return true;
}

static enum leme_public_status
serialize(struct timao_client *client, struct timao_client_message *message,
          const struct leme_public_value *expression,
          struct leme_public_text subscription,
          struct leme_control_frame **out) {
  const bool hello = message->failure.operation == TIMAO_CLIENT_HELLO;
  const size_t maximum = hello ? 65536 : client->remote.request_bytes;
  struct leme_json json = {0};
  leme_json_init_budget(&json, client->account, maximum + 1);
  leme_json_object_begin(&json);
  leme_json_key(&json, "version");
  leme_json_integer(&json, 1);
  leme_json_key(&json, "id");
  leme_json_string_n(&json, message->failure.request_id.data,
                     message->failure.request_id.length);
  leme_json_key(&json, "op");
  static const char *const names[] = {"hello", "query", "act", "watch",
                                      "unwatch"};
  leme_json_string(&json, names[message->failure.operation]);
  enum leme_public_status status = LEME_PUBLIC_OK;
  if (!hello) {
    leme_json_key(&json, "instance");
    leme_json_string_n(&json, client->instance.data, client->instance.length);
    if (message->failure.operation == TIMAO_CLIENT_UNWATCH) {
      leme_json_key(&json, "subscription");
      leme_json_string_n(&json, subscription.data, subscription.length);
    } else {
      leme_json_key(&json, "expr");
      status = leme_public_write_json(expression, &json);
    }
  }
  leme_json_object_end(&json);
  if (status == LEME_PUBLIC_OK)
    status = json_status(&json);
  if (status == LEME_PUBLIC_OK && json.length > maximum)
    status = LEME_PUBLIC_LIMIT;
  if (status == LEME_PUBLIC_OK && !hello) {
    size_t remaining = client->remote.expression_nodes;
    if (client->remote.json_depth < 2 ||
        (expression != NULL &&
         (!depth_fits(expression, &client->remote, 2) ||
          !expression_fits(expression, &client->remote, 1, &remaining))))
      status = LEME_PUBLIC_LIMIT;
  }
  if (status == LEME_PUBLIC_OK) {
    leme_json_append(&json, "\n", 1);
    status = json_status(&json);
  }
  if (status == LEME_PUBLIC_OK) {
    const enum leme_control_code code =
        leme_control_frame_create(client->account, json.data, json.length, out);
    if (code != LEME_CONTROL_OK)
      status = code == LEME_CONTROL_OUT_OF_MEMORY ? LEME_PUBLIC_OOM
                                                  : LEME_PUBLIC_LIMIT;
  }
  leme_json_finish(&json);
  return status;
}

enum leme_public_status
timao_client_admit(struct timao_client *client,
                   enum timao_client_operation operation,
                   const struct leme_public_value *expression,
                   struct leme_public_text subscription, uint64_t watch,
                   struct timao_client_ticket *ticket) {
  if (client->serial == UINT64_MAX ||
      client->pending_count >= client->remote.outstanding)
    return LEME_PUBLIC_LIMIT;
  struct timao_client_pending *slot = NULL;
  for (size_t i = 0; i < 16; ++i)
    if (client->pending[i].completion == NULL) {
      slot = &client->pending[i];
      break;
    }
  if (slot == NULL)
    return LEME_PUBLIC_LIMIT;
  struct timao_client_message *message =
      leme_control_alloc(client->account, sizeof(*message));
  if (message == NULL)
    return timao_client_allocation_status();
  *message = (struct timao_client_message){
      .kind = TIMAO_CLIENT_REPLY,
      .ticket = {client->epoch, client->serial + 1},
      .watch = watch,
      .failure = {.operation = operation}};
  const int n = snprintf(message->request_id, sizeof(message->request_id),
                         "c:%" PRIu64 ":%" PRIu64, message->ticket.epoch,
                         message->ticket.serial);
  enum leme_public_status status = LEME_PUBLIC_LIMIT;
  struct leme_control_frame *frame = NULL;
  if (n < 0 || (size_t)n >= sizeof(message->request_id))
    goto done;
  message->failure.request_id =
      (struct leme_public_text){message->request_id, (size_t)n};
  status = copy_instance(client, message, client->instance);
  if (status != LEME_PUBLIC_OK)
    goto done;
  status = serialize(client, message, expression, subscription, &frame);
  if (status != LEME_PUBLIC_OK)
    goto done;
  *slot = (struct timao_client_pending){
      .completion = message,
      .frame = frame,
      .length = leme_control_frame_length(frame),
      .internal = client->recovering && operation != TIMAO_CLIENT_HELLO,
      .deadline = timao_client_deadline(
          client->clock.now_ms(client->clock.context),
          operation == TIMAO_CLIENT_HELLO ? client->limits.handshake_ms
                                          : client->limits.request_ms)};
  ++client->pending_count;
  ++client->serial;
  *ticket = message->ticket;
  return LEME_PUBLIC_OK;
done:
  leme_control_frame_destroy(frame);
  timao_client_message_destroy(message);
  return status;
}

enum leme_public_status timao_client_opened(struct timao_client *client,
                                            uint64_t epoch) {
  if (client == NULL)
    return LEME_PUBLIC_INVALID;
  if (epoch != client->epoch || client->state != TIMAO_CLIENT_CONNECTING)
    return LEME_PUBLIC_UNAVAILABLE;
  if (client->clock.now_ms(client->clock.context) >= client->deadline) {
    timao_client_fail(client, TIMAO_CLIENT_TIMEOUT);
    return LEME_PUBLIC_UNAVAILABLE;
  }
  client->remote = leme_control_limits_default();
  const enum leme_control_code code = leme_control_framer_create(
      client->account, client->remote.response_bytes, &client->framer);
  enum leme_public_status status = code == LEME_CONTROL_OK ? LEME_PUBLIC_OK
                                   : code == LEME_CONTROL_OUT_OF_MEMORY
                                       ? LEME_PUBLIC_OOM
                                       : LEME_PUBLIC_LIMIT;
  struct timao_client_ticket ticket = {0};
  if (status == LEME_PUBLIC_OK)
    status = timao_client_admit(client, TIMAO_CLIENT_HELLO, NULL,
                                (struct leme_public_text){0}, 0, &ticket);
  if (status != LEME_PUBLIC_OK) {
    timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
    return status;
  }
  client->state = TIMAO_CLIENT_NEGOTIATING;
  client->deadline = timao_client_deadline(
      client->clock.now_ms(client->clock.context), client->limits.handshake_ms);
  return LEME_PUBLIC_OK;
}

bool timao_client_ready(const struct timao_client *client) {
  return client != NULL && client->state == TIMAO_CLIENT_READY &&
         !client->recovering && !client->shutting_down;
}

enum leme_public_status
timao_client_request(struct timao_client *client,
                     enum timao_client_operation operation,
                     const struct leme_public_value *expression,
                     struct timao_client_ticket *ticket) {
  if (ticket == NULL)
    return LEME_PUBLIC_INVALID;
  *ticket = (struct timao_client_ticket){0};
  if (client == NULL || expression == NULL ||
      (operation != TIMAO_CLIENT_QUERY && operation != TIMAO_CLIENT_ACT))
    return LEME_PUBLIC_INVALID;
  timao_client_tick(client);
  if (!timao_client_ready(client))
    return LEME_PUBLIC_UNAVAILABLE;
  if ((operation == TIMAO_CLIENT_QUERY && !client->query_capability) ||
      (operation == TIMAO_CLIENT_ACT && !client->action_capability))
    return LEME_PUBLIC_UNAVAILABLE;
  return timao_client_admit(client, operation, expression,
                            (struct leme_public_text){0}, 0, ticket);
}

enum leme_public_status
timao_client_peek_write(struct timao_client *client,
                        struct timao_client_ticket *ticket,
                        struct leme_public_text *bytes) {
  if (client == NULL || ticket == NULL || bytes == NULL)
    return LEME_PUBLIC_INVALID;
  *ticket = (struct timao_client_ticket){0};
  *bytes = (struct leme_public_text){0};
  timao_client_tick(client);
  struct timao_client_pending *p = first_pending(client, true);
  if (p != NULL) {
    *ticket = p->completion->ticket;
    *bytes = (struct leme_public_text){
        leme_control_frame_bytes(p->frame) + p->offset, p->length - p->offset};
    p->offered = true;
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status timao_client_written(struct timao_client *client,
                                             struct timao_client_ticket ticket,
                                             size_t accepted_bytes) {
  if (client == NULL)
    return LEME_PUBLIC_INVALID;
  struct timao_client_pending *p = first_pending(client, true);
  if (p == NULL || !p->offered || p->completion->ticket.epoch != ticket.epoch ||
      p->completion->ticket.serial != ticket.serial)
    return LEME_PUBLIC_INVALID;
  if (accepted_bytes > p->length - p->offset) {
    timao_client_fail(client, TIMAO_CLIENT_AMBIGUOUS_IO);
    return LEME_PUBLIC_INVALID;
  }
  p->offset += accepted_bytes;
  if (p->offset == p->length) {
    leme_control_frame_destroy(p->frame);
    p->frame = NULL;
  }
  return LEME_PUBLIC_OK;
}

static bool limit_field(const struct leme_public_value *limits,
                        const char *name, size_t maximum, size_t *out) {
  int64_t n = 0;
  if (leme_public_as_integer(field(limits, name), &n) != LEME_PUBLIC_OK ||
      n <= 0 || (uint64_t)n > maximum)
    return false;
  *out = (size_t)n;
  return true;
}

static bool capability(const struct leme_public_value *caps, const char *name) {
  for (size_t i = 0; i < leme_public_length(caps); ++i)
    if (is_text(leme_public_at(caps, i), name))
      return true;
  return false;
}

static enum leme_public_status negotiate(struct timao_client *client,
                                         struct leme_control_document *doc) {
  const struct leme_public_value *root = leme_control_document_value(doc);
  const struct leme_public_value *value = field(root, "value");
  const struct leme_public_value *caps = field(value, "capabilities");
  const struct leme_public_value *limits = field(value, "limits");
  int64_t version = 0;
  struct leme_public_text instance = {0};
  struct leme_control_limits remote = leme_control_limits_default();
  if (leme_public_as_integer(field(value, "api_version"), &version) !=
          LEME_PUBLIC_OK ||
      version != 1 ||
      leme_public_kind(field(value, "version")) != LEME_PUBLIC_STRING ||
      leme_public_kind(caps) != LEME_PUBLIC_ARRAY ||
      !capability(caps, "hello") ||
      !limit_field(limits, "request_bytes", remote.request_bytes,
                   &remote.request_bytes) ||
      !limit_field(limits, "response_bytes", remote.response_bytes,
                   &remote.response_bytes) ||
      !limit_field(limits, "outstanding", remote.outstanding,
                   &remote.outstanding) ||
      !limit_field(limits, "json_depth", remote.json_depth,
                   &remote.json_depth) ||
      leme_public_as_text(field(root, "instance"), &instance) !=
          LEME_PUBLIC_OK ||
      leme_public_kind(field(root, "revision")) != LEME_PUBLIC_NULL)
    return LEME_PUBLIC_INVALID;
  for (size_t i = 0; i < leme_public_length(caps); ++i)
    if (leme_public_kind(leme_public_at(caps, i)) != LEME_PUBLIC_STRING)
      return LEME_PUBLIC_INVALID;
  struct optional_limit {
    const char *name;
    size_t *value;
  } optional[] = {{"expression_depth", &remote.expression_depth},
                  {"expression_nodes", &remote.expression_nodes},
                  {"field_depth", &remote.field_depth},
                  {"targets", &remote.targets},
                  {"work_units", &remote.work_units},
                  {"output_bytes", &remote.output_bytes},
                  {"retained_bytes", &remote.retained_bytes},
                  {"snapshot_bytes", &remote.snapshot_bytes},
                  {"total_bytes", &remote.total_bytes}};
  for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); ++i)
    if (field(limits, optional[i].name) != NULL &&
        !limit_field(limits, optional[i].name, *optional[i].value,
                     optional[i].value))
      return LEME_PUBLIC_INVALID;
  if (field(limits, "deadline_ns") != NULL) {
    int64_t deadline = 0;
    if (leme_public_as_integer(field(limits, "deadline_ns"), &deadline) !=
            LEME_PUBLIC_OK ||
        deadline <= 0 || (uint64_t)deadline > remote.deadline_ns)
      return LEME_PUBLIC_INVALID;
    remote.deadline_ns = (uint64_t)deadline;
  }
  const bool watch = capability(caps, "watch") && capability(caps, "unwatch");
  if (watch || field(limits, "subscriptions") != NULL) {
    if (!limit_field(limits, "subscriptions", remote.subscriptions,
                     &remote.subscriptions))
      return LEME_PUBLIC_INVALID;
  } else {
    remote.subscriptions = 0;
  }
  const enum leme_public_status status =
      copy_instance(client, client->emergency, instance);
  if (status != LEME_PUBLIC_OK)
    return status;
  client->remote = remote;
  client->query_capability = capability(caps, "query");
  client->action_capability = capability(caps, "act");
  client->watch_capability = watch;
  client->hello = doc;
  client->instance = instance;
  client->state = TIMAO_CLIENT_READY;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status reply(struct timao_client *client,
                                     struct leme_control_document **document) {
  const struct leme_public_value *root = leme_control_document_value(*document);
  struct leme_public_text id = {0};
  struct leme_public_text instance = {0};
  bool ok = false;
  if (!is_text(field(root, "type"), "reply") ||
      leme_public_as_text(field(root, "id"), &id) != LEME_PUBLIC_OK ||
      leme_public_as_text(field(root, "instance"), &instance) !=
          LEME_PUBLIC_OK ||
      leme_public_as_bool(field(root, "ok"), &ok) != LEME_PUBLIC_OK)
    return LEME_PUBLIC_INVALID;
  struct timao_client_pending *p = NULL;
  for (size_t i = 0; i < 16; ++i)
    if (client->pending[i].completion != NULL &&
        equal_text(client->pending[i].completion->failure.request_id, id)) {
      p = &client->pending[i];
      break;
    }
  if (p == NULL || p->offset != p->length ||
      (client->state == TIMAO_CLIENT_READY &&
       !equal_text(instance, client->instance)))
    return LEME_PUBLIC_INVALID;
  if (client->helper_ticket.serial != 0 &&
      p->completion->ticket.serial == client->helper_ticket.serial) {
    const enum leme_public_status status =
        timao_client_helper_reply(client, root, ok);
    if (status != LEME_PUBLIC_OK)
      return status;
    clear_pending(client, p);
    return LEME_PUBLIC_OK;
  }
  if (client->status_ticket.serial != 0 &&
      p->completion->ticket.serial == client->status_ticket.serial) {
    if (!ok)
      return LEME_PUBLIC_INVALID;
    const enum leme_public_status status =
        timao_client_recovery_status(client, field(root, "value"));
    if (status != LEME_PUBLIC_OK)
      return status;
    clear_pending(client, p);
    return LEME_PUBLIC_OK;
  }
  if (p->completion->failure.operation == TIMAO_CLIENT_WATCH ||
      p->completion->failure.operation == TIMAO_CLIENT_UNWATCH) {
    const enum leme_public_status status =
        timao_client_watch_reply(client, p, document, ok);
    if (status != LEME_PUBLIC_OK)
      return status;
    clear_pending(client, p);
    timao_client_watches_pump(client);
    return LEME_PUBLIC_OK;
  }
  if (p->completion->failure.operation == TIMAO_CLIENT_HELLO && ok) {
    const enum leme_public_status status = negotiate(client, *document);
    if (status != LEME_PUBLIC_OK)
      return status;
    *document = NULL;
    clear_pending(client, p);
    return LEME_PUBLIC_OK;
  }
  const bool hello = p->completion->failure.operation == TIMAO_CLIENT_HELLO;
  p->completion->document = *document;
  if (!timao_client_deliver(client, p->completion)) {
    p->completion->document = NULL;
    return LEME_PUBLIC_LIMIT;
  }
  *document = NULL;
  p->completion = NULL;
  clear_pending(client, p);
  if (hello)
    timao_client_fail(client, TIMAO_CLIENT_UNSUPPORTED);
  return LEME_PUBLIC_OK;
}

enum leme_public_status timao_client_feed(struct timao_client *client,
                                          uint64_t epoch,
                                          struct leme_public_text bytes,
                                          size_t *consumed) {
  if (consumed == NULL)
    return LEME_PUBLIC_INVALID;
  *consumed = 0;
  if (client == NULL || (bytes.length != 0 && bytes.data == NULL))
    return LEME_PUBLIC_INVALID;
  if (epoch != client->epoch || (client->state != TIMAO_CLIENT_READY &&
                                 client->state != TIMAO_CLIENT_NEGOTIATING))
    return LEME_PUBLIC_UNAVAILABLE;
  while (*consumed < bytes.length) {
    const size_t pending = leme_control_framer_pending_bytes(client->framer);
    const size_t available = client->limits.queued_bytes - client->queued_bytes;
    const size_t input = bytes.length - *consumed;
    const size_t maximum_scan = client->remote.response_bytes + 1;
    const size_t scan = input < maximum_scan ? input : maximum_scan;
    const char *newline = memchr(bytes.data + *consumed, '\n', scan);
    if (pending > client->remote.response_bytes || pending > available ||
        (newline == NULL &&
         (input > available - pending ||
          input > client->remote.response_bytes - pending))) {
      timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
      return LEME_PUBLIC_LIMIT;
    }
    struct leme_control_frame *frame = NULL;
    size_t used = 0;
    const enum leme_control_code code =
        leme_control_framer_feed(client->framer, bytes.data + *consumed,
                                 bytes.length - *consumed, &used, &frame);
    *consumed += used;
    enum leme_public_status status = code == LEME_CONTROL_OK ? LEME_PUBLIC_OK
                                     : code == LEME_CONTROL_OUT_OF_MEMORY
                                         ? LEME_PUBLIC_OOM
                                         : LEME_PUBLIC_LIMIT;
    struct leme_control_document *doc = NULL;
    if (status == LEME_PUBLIC_OK && frame != NULL) {
      const struct leme_public_text record = {leme_control_frame_bytes(frame),
                                              leme_control_frame_length(frame)};
      size_t metadata = sizeof(struct timao_client_message) +
                        leme_control_allocation_overhead();
      for (size_t i = 0; i < 16; ++i) {
        const size_t bytes_needed =
            timao_client_message_metadata(client->pending[i].completion);
        if (bytes_needed > metadata)
          metadata = bytes_needed;
      }
      struct timao_client_limits decode_limits = client->limits;
      decode_limits.queued_bytes =
          timao_client_delivery_arena(client, metadata);
      status = record.length > client->remote.response_bytes ||
                       decode_limits.queued_bytes == 0
                   ? LEME_PUBLIC_LIMIT
                   : timao_client_record_decode(client->account, record,
                                                &decode_limits, &doc, NULL);
      if (status == LEME_PUBLIC_OK) {
        const struct leme_public_value *root = leme_control_document_value(doc);
        status = is_text(field(root, "type"), "event")
                     ? timao_client_watch_event(client, root)
                     : reply(client, &doc);
      }
    }
    leme_control_document_destroy(doc);
    leme_control_frame_destroy(frame);
    if (status != LEME_PUBLIC_OK) {
      timao_client_fail(client, status == LEME_PUBLIC_INVALID
                                    ? TIMAO_CLIENT_PROTOCOL
                                    : TIMAO_CLIENT_RESOURCE);
      return status;
    }
    timao_client_recovery_pump(client);
    if (client->state == TIMAO_CLIENT_CLOSING)
      break;
  }
  return LEME_PUBLIC_OK;
}

void timao_client_lost(struct timao_client *client,
                       struct timao_client_loss loss) {
  if (client != NULL && loss.epoch == client->epoch)
    timao_client_fail(client, loss.cause);
}

void timao_client_closed(struct timao_client *client, uint64_t epoch) {
  if (client == NULL || epoch != client->epoch ||
      client->state != TIMAO_CLIENT_CLOSING)
    return;
  client->state = TIMAO_CLIENT_IDLE;
  leme_control_document_destroy(client->hello);
  client->hello = NULL;
  client->instance = (struct leme_public_text){0};
  if (client->emergency != NULL) {
    leme_control_free(client->emergency->instance_copy);
    client->emergency->instance_copy = NULL;
    client->emergency->failure.instance = (struct leme_public_text){0};
  }
}

enum timao_client_message_kind
timao_client_message_kind(const struct timao_client_message *message) {
  return message != NULL ? message->kind : TIMAO_CLIENT_FAILURE;
}
const struct leme_public_value *
timao_client_message_value(const struct timao_client_message *message) {
  return message != NULL ? leme_control_document_value(message->document)
                         : NULL;
}
struct timao_client_ticket
timao_client_message_ticket(const struct timao_client_message *message) {
  return message != NULL ? message->ticket : (struct timao_client_ticket){0};
}
uint64_t
timao_client_message_watch(const struct timao_client_message *message) {
  return message != NULL ? message->watch : 0;
}
void timao_client_shutdown(struct timao_client *client) {
  if (client == NULL || client->shutting_down)
    return;
  client->shutting_down = true;
  client->retry_pending = false;
  client->shutdown_due = timao_client_deadline(
      client->clock.now_ms(client->clock.context), client->limits.shutdown_ms);
  if (client->state != TIMAO_CLIENT_READY) {
    timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
    timao_client_watches_clear(client);
    return;
  }
  for (size_t i = 0; i < 16; ++i) {
    const struct timao_client_message *message = client->pending[i].completion;
    if (message != NULL && message->failure.operation != TIMAO_CLIENT_UNWATCH) {
      timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
      return;
    }
  }
  client->recovering = false;
  for (size_t i = 0; i < 32; ++i)
    if (client->watches[i].id != 0)
      timao_client_cancel(client, client->watches[i].id);
  timao_client_shutdown_tick(client);
}
