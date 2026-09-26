#include "timao/client-internal.h"
#include "control/memory.h"

#include <errno.h>

uint64_t timao_client_deadline(uint64_t now, uint64_t delay) {
  return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}

enum leme_public_status timao_client_allocation_status(void) {
  return errno == ENOSPC || errno == EOVERFLOW ? LEME_PUBLIC_LIMIT
                                               : LEME_PUBLIC_OOM;
}

static enum leme_public_status reserve_emergency(struct timao_client *client) {
  if (client->emergency != NULL)
    return LEME_PUBLIC_OK;
  client->emergency =
      leme_control_alloc(client->account, sizeof(*client->emergency));
  if (client->emergency == NULL)
    return timao_client_allocation_status();
  *client->emergency = (struct timao_client_message){
      .kind = TIMAO_CLIENT_FAILURE,
      .failure = {.operation = TIMAO_CLIENT_HELLO, .effects_known_none = true}};
  return LEME_PUBLIC_OK;
}

enum leme_public_status
timao_client_create(struct leme_public_budget *account,
                    const struct timao_client_limits *limits,
                    const struct timao_client_clock *clock,
                    struct timao_client **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  struct timao_client_limits selected = {0};
  if (account == NULL || clock == NULL || clock->now_ms == NULL ||
      timao_client_limits_resolve(limits, &selected) != LEME_PUBLIC_OK)
    return LEME_PUBLIC_INVALID;
  struct leme_public_budget *owner = NULL;
  enum leme_public_status status =
      leme_public_budget_child(account, selected.total_bytes, &owner);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct timao_client *client = leme_control_alloc(owner, sizeof(*client));
  if (client == NULL) {
    status = timao_client_allocation_status();
    leme_public_budget_unref(owner);
    return status;
  }
  *client = (struct timao_client){
      .account = owner, .limits = selected, .clock = *clock};
  status = reserve_emergency(client);
  if (status != LEME_PUBLIC_OK) {
    timao_client_destroy(client);
    return status;
  }
  *out = client;
  return LEME_PUBLIC_OK;
}

void timao_client_message_destroy(struct timao_client_message *message) {
  if (message == NULL)
    return;
  leme_control_document_destroy(message->document);
  leme_control_free(message->instance_copy);
  leme_control_free(message);
}

void timao_client_destroy(struct timao_client *client) {
  if (client == NULL)
    return;
  struct timao_client_message *message = NULL;
  while ((message = timao_client_take(client)) != NULL)
    timao_client_message_destroy(message);
  timao_client_message_destroy(client->emergency);
  timao_client_requests_clear(client);
  timao_client_watches_clear(client);
  leme_control_document_destroy(client->hello);
  leme_control_framer_destroy(client->framer);
  struct leme_public_budget *owner = client->account;
  leme_control_free(client);
  leme_public_budget_unref(owner);
}

enum leme_public_status timao_client_start(struct timao_client *client) {
  if (client == NULL)
    return LEME_PUBLIC_INVALID;
  if (client->state != TIMAO_CLIENT_IDLE || client->retry_pending ||
      client->shutting_down)
    return LEME_PUBLIC_UNAVAILABLE;
  if (client->epoch == UINT64_MAX)
    return LEME_PUBLIC_LIMIT;
  const enum leme_public_status status = reserve_emergency(client);
  if (status != LEME_PUBLIC_OK)
    return status;
  ++client->epoch;
  client->emergency->ticket.epoch = client->epoch;
  client->state = TIMAO_CLIENT_CONNECTING;
  client->deadline = timao_client_deadline(
      client->clock.now_ms(client->clock.context), client->limits.handshake_ms);
  return LEME_PUBLIC_OK;
}

bool timao_client_wants_connect(const struct timao_client *client) {
  return client != NULL && client->state == TIMAO_CLIENT_CONNECTING;
}
uint64_t timao_client_connect_epoch(const struct timao_client *client) {
  return client != NULL ? client->epoch : 0;
}
bool timao_client_wants_close(const struct timao_client *client) {
  return client != NULL && client->state == TIMAO_CLIENT_CLOSING;
}

size_t
timao_client_message_metadata(const struct timao_client_message *message) {
  const size_t object = leme_control_allocation_bytes(message);
  const size_t instance =
      message != NULL ? leme_control_allocation_bytes(message->instance_copy)
                      : 0;
  return object > SIZE_MAX - instance ? SIZE_MAX : object + instance;
}

size_t timao_client_delivery_arena(const struct timao_client *client,
                                   size_t metadata) {
  const size_t available = client->limits.queued_bytes - client->queued_bytes;
  const size_t overhead = leme_control_document_overhead();
  if (metadata >= available || overhead >= available - metadata)
    return 0;
  return available - metadata - overhead;
}

bool timao_client_deliver(struct timao_client *client,
                          struct timao_client_message *message) {
  const size_t metadata = timao_client_message_metadata(message);
  const size_t data = leme_control_document_bytes(message->document);
  const size_t available = client->limits.queued_bytes - client->queued_bytes;
  if (metadata > available || data > available - metadata)
    return false;
  message->queued_bytes = metadata + data;
  client->queued_bytes += message->queued_bytes;
  timao_client_enqueue(client, message);
  return true;
}

void timao_client_enqueue(struct timao_client *client,
                          struct timao_client_message *message) {
  message->next = NULL;
  if (client->tail != NULL)
    client->tail->next = message;
  else
    client->head = message;
  client->tail = message;
}

void timao_client_transport_notice(struct timao_client *client,
                                   struct timao_client_message *message) {
  if (client->recovering) {
    for (const struct timao_client_message *queued = client->head;
         queued != NULL; queued = queued->next) {
      if (queued->watch == 0 && queued->kind == TIMAO_CLIENT_FAILURE &&
          queued->failure.operation == TIMAO_CLIENT_HELLO &&
          queued->document == NULL) {
        timao_client_message_destroy(message);
        return;
      }
    }
  }
  timao_client_enqueue(client, message);
}

void timao_client_fail(struct timao_client *client,
                       enum timao_client_cause cause) {
  if (client->state == TIMAO_CLIENT_IDLE ||
      client->state == TIMAO_CLIENT_CLOSING)
    return;
  client->state = TIMAO_CLIENT_CLOSING;
  const bool had_pending = client->pending_count != 0;
  timao_client_requests_fail(client, cause);
  timao_client_watches_lost(client, cause);
  leme_control_framer_destroy(client->framer);
  client->framer = NULL;
  if (!had_pending && client->emergency != NULL) {
    client->emergency->failure.cause = cause;
    timao_client_transport_notice(client, client->emergency);
    client->emergency = NULL;
  }
  timao_client_recovery_lost(client, cause);
}

void timao_client_tick(struct timao_client *client) {
  if (client == NULL)
    return;
  if ((client->state == TIMAO_CLIENT_CONNECTING ||
       client->state == TIMAO_CLIENT_NEGOTIATING) &&
      client->clock.now_ms(client->clock.context) >= client->deadline)
    timao_client_fail(client, TIMAO_CLIENT_TIMEOUT);
  if (client->state == TIMAO_CLIENT_READY)
    timao_client_requests_tick(client,
                               client->clock.now_ms(client->clock.context));
  timao_client_watches_pump(client);
  timao_client_shutdown_tick(client);
  timao_client_recovery_tick(client);
}

struct delivery_selection {
  struct timao_client_ticket ticket;
  uint64_t watch;
  const uint64_t *handlers;
  size_t handler_count;
  bool dispatch;
};

static bool dispatchable(const struct timao_client_message *message,
                         struct delivery_selection selection) {
  if (message->watch == 0 || message->kind == TIMAO_CLIENT_FAILURE)
    return true;
  const struct leme_public_value *value = timao_client_message_value(message);
  if (value != NULL &&
      leme_public_get(value, LEME_PUBLIC_TEXT("error")) != NULL)
    return true;
  for (size_t i = 0; i < selection.handler_count; ++i)
    if (message->watch == selection.handlers[i])
      return true;
  return false;
}

static struct timao_client_message *
take_selected(struct timao_client *client,
              struct delivery_selection selection) {
  if (client == NULL)
    return NULL;
  struct timao_client_message *previous = NULL;
  for (struct timao_client_message *message = client->head; message != NULL;
       message = message->next) {
    const bool matches =
        selection.dispatch     ? dispatchable(message, selection)
        : selection.watch != 0 ? message->watch == selection.watch
        : selection.ticket.serial != 0
            ? message->ticket.epoch == selection.ticket.epoch &&
                  message->ticket.serial == selection.ticket.serial
            : true;
    if (!matches) {
      previous = message;
      continue;
    }
    if (previous != NULL)
      previous->next = message->next;
    else
      client->head = message->next;
    if (client->tail == message)
      client->tail = previous;
    message->next = NULL;
    client->queued_bytes -= message->queued_bytes;
    message->queued_bytes = 0;
    return message;
  }
  return NULL;
}

struct timao_client_message *
timao_client_take_handlers(struct timao_client *client, const uint64_t *watches,
                           size_t count) {
  if (count > 32 || (count != 0 && watches == NULL))
    return NULL;
  return take_selected(client,
                       (struct delivery_selection){.handlers = watches,
                                                   .handler_count = count,
                                                   .dispatch = true});
}

struct timao_client_message *timao_client_take(struct timao_client *client) {
  return take_selected(client, (struct delivery_selection){0});
}

struct timao_client_message *
timao_client_take_reply(struct timao_client *client,
                        struct timao_client_ticket ticket) {
  if (ticket.epoch == 0 || ticket.serial == 0)
    return NULL;
  return take_selected(client, (struct delivery_selection){.ticket = ticket});
}

struct timao_client_message *
timao_client_take_watch(struct timao_client *client, uint64_t watch) {
  return watch != 0 ? take_selected(client,
                                    (struct delivery_selection){.watch = watch})
                    : NULL;
}

bool timao_client_request_identity(const struct timao_client *client,
                                   struct timao_client_ticket ticket,
                                   struct leme_public_text *request_id,
                                   struct leme_public_text *instance) {
  if (request_id == NULL || instance == NULL)
    return false;
  *request_id = (struct leme_public_text){0};
  *instance = (struct leme_public_text){0};
  if (client == NULL || ticket.epoch == 0 || ticket.serial == 0)
    return false;
  for (size_t i = 0; i < 16; ++i) {
    const struct timao_client_message *message = client->pending[i].completion;
    if (message != NULL && message->ticket.epoch == ticket.epoch &&
        message->ticket.serial == ticket.serial) {
      *request_id = message->failure.request_id;
      *instance = message->failure.instance;
      return true;
    }
  }
  return false;
}

const struct timao_client_failure *
timao_client_message_failure(const struct timao_client_message *message) {
  return message != NULL && message->kind == TIMAO_CLIENT_FAILURE
             ? &message->failure
             : NULL;
}
