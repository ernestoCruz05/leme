#include "timao/client-internal.h"

#include <string.h>

bool timao_client_recovery_interested(const struct timao_client *client) {
  for (size_t i = 0; i < 32; ++i) {
    const struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id != 0 && w->acknowledged && w->interest && !w->canceled)
      return true;
  }
  return false;
}

static uint32_t draw(struct timao_client *client) {
  if (client->clock.random_u32 != NULL)
    return client->clock.random_u32(client->clock.context);
  uint32_t state = client->random_state;
  if (state == 0)
    state = (uint32_t)client->clock.now_ms(client->clock.context) ^
            UINT32_C(2463534242);
  if (state == 0)
    state = 1;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  client->random_state = state;
  return state;
}

void timao_client_recovery_lost(struct timao_client *client,
                                enum timao_client_cause cause) {
  const bool retryable =
      cause == TIMAO_CLIENT_EOF || cause == TIMAO_CLIENT_IO_ERROR ||
      cause == TIMAO_CLIENT_AMBIGUOUS_IO || cause == TIMAO_CLIENT_TIMEOUT;
  client->retry_pending = false;
  client->recovery_status_known = false;
  client->status_ticket = (struct timao_client_ticket){0};
  client->helper_ticket = (struct timao_client_ticket){0};
  client->helper_state = TIMAO_HELPER_NONE;
  client->helper_length = 0;
  client->helper_sequence = 0;
  if (client->shutting_down || !retryable ||
      !timao_client_recovery_interested(client)) {
    client->recovering = false;
    return;
  }
  const uint64_t nominal = UINT64_C(250) << client->retry_step;
  const uint64_t spread = nominal / 5;
  const uint64_t jitter = ((uint64_t)draw(client) * (spread * 2 + 1)) >> 32;
  const uint64_t computed = nominal - spread + jitter;
  const uint64_t delay = computed > 8000 ? 8000 : computed;
  client->retry_due =
      timao_client_deadline(client->clock.now_ms(client->clock.context), delay);
  if (client->retry_step < 5)
    ++client->retry_step;
  client->retry_pending = true;
  client->recovering = true;
}

void timao_client_recovery_tick(struct timao_client *client) {
  if (client->shutting_down || !timao_client_recovery_interested(client)) {
    client->retry_pending = false;
    if (client->recovering && (client->state == TIMAO_CLIENT_CONNECTING ||
                               client->state == TIMAO_CLIENT_NEGOTIATING))
      timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
    if (client->recovering && client->helper_state != TIMAO_HELPER_NONE) {
      client->recovery_locked = false;
      timao_client_recovery_pump(client);
    } else {
      client->recovering = false;
    }
    return;
  }
  timao_client_recovery_pump(client);
  if (!client->retry_pending || client->state != TIMAO_CLIENT_IDLE ||
      client->clock.now_ms(client->clock.context) < client->retry_due)
    return;
  client->retry_pending = false;
  const enum leme_public_status status = timao_client_start(client);
  if (status != LEME_PUBLIC_OK) {
    client->recovering = false;
    timao_client_watches_lost(client, TIMAO_CLIENT_RESOURCE);
  }
}

static enum leme_public_status
status_request(struct timao_client *client,
               enum timao_client_operation operation) {
  const struct leme_control_limits limits = leme_control_limits_default();
  struct leme_control_document *doc = NULL;
  const enum leme_control_code code = leme_control_decode(
      client->account, LEME_PUBLIC_TEXT("{\"call\":\"status\",\"args\":[]}"),
      &limits, NULL, &doc, NULL);
  if (code != LEME_CONTROL_OK)
    return code == LEME_CONTROL_OUT_OF_MEMORY ? LEME_PUBLIC_OOM
                                              : LEME_PUBLIC_LIMIT;
  struct timao_client_ticket *ticket = operation == TIMAO_CLIENT_QUERY
                                           ? &client->status_ticket
                                           : &client->helper_ticket;
  const enum leme_public_status status =
      timao_client_admit(client, operation, leme_control_document_value(doc),
                         (struct leme_public_text){0}, 0, ticket);
  leme_control_document_destroy(doc);
  return status;
}

enum leme_public_status
timao_client_recovery_status(struct timao_client *client,
                             const struct leme_public_value *value) {
  bool locked = false;
  if (leme_public_as_bool(leme_public_get(value, LEME_PUBLIC_TEXT("locked")),
                          &locked) != LEME_PUBLIC_OK)
    return LEME_PUBLIC_INVALID;
  client->recovery_status_known = true;
  client->recovery_locked = locked;
  client->status_ticket = (struct timao_client_ticket){0};
  return LEME_PUBLIC_OK;
}

void timao_client_recovery_pump(struct timao_client *client) {
  if (!client->recovering || client->state != TIMAO_CLIENT_READY)
    return;
  if (!client->query_capability || !client->watch_capability) {
    timao_client_watches_lost(client, TIMAO_CLIENT_UNSUPPORTED);
    client->recovering = false;
    return;
  }
  if (!client->recovery_status_known) {
    if (client->status_ticket.serial == 0 &&
        client->pending_count < client->remote.outstanding &&
        status_request(client, TIMAO_CLIENT_QUERY) != LEME_PUBLIC_OK)
      timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
    return;
  }
  bool needs_helper = false;
  for (size_t i = 0; i < 32; ++i) {
    const struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id != 0 && w->interest && !w->canceled && w->lost && w->sensitive &&
        w->subscription_length == 0)
      needs_helper = true;
  }
  if (client->helper_state == TIMAO_HELPER_NONE && client->recovery_locked &&
      needs_helper) {
    if (client->pending_count >= client->remote.outstanding)
      return;
    if (status_request(client, TIMAO_CLIENT_WATCH) != LEME_PUBLIC_OK) {
      timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
      return;
    }
    client->helper_state = TIMAO_HELPER_REGISTERING;
  }
  if (client->helper_state == TIMAO_HELPER_ACTIVE &&
      (!client->recovery_locked || !needs_helper)) {
    if (client->pending_count >= client->remote.outstanding)
      return;
    if (timao_client_admit(
            client, TIMAO_CLIENT_UNWATCH, NULL,
            (struct leme_public_text){client->helper_subscription,
                                      client->helper_length},
            0, &client->helper_ticket) != LEME_PUBLIC_OK) {
      timao_client_fail(client, TIMAO_CLIENT_RESOURCE);
      return;
    }
    client->helper_state = TIMAO_HELPER_REMOVING;
  }
  if (client->helper_state == TIMAO_HELPER_REMOVING)
    return;
  size_t occupied = client->helper_state == TIMAO_HELPER_NONE ? 0 : 1;
  for (size_t i = 0; i < 32; ++i) {
    const struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id != 0 && (w->subscription_length != 0 || w->pending_registration))
      ++occupied;
  }
  uint64_t previous = 0;
  for (size_t n = 0; n < 32; ++n) {
    struct timao_client_watch_slot *w = NULL;
    for (size_t i = 0; i < 32; ++i) {
      struct timao_client_watch_slot *candidate = &client->watches[i];
      if (candidate->id > previous && (w == NULL || candidate->id < w->id))
        w = candidate;
    }
    if (w == NULL)
      break;
    previous = w->id;
    if (!w->lost || w->canceled || !w->interest || w->pending_registration ||
        w->subscription_length != 0 ||
        (w->sensitive && client->recovery_locked))
      continue;
    if (occupied >= client->remote.subscriptions) {
      timao_client_watch_terminate(client, w, TIMAO_CLIENT_RESOURCE);
      continue;
    }
    if (client->pending_count >= client->remote.outstanding)
      return;
    struct timao_client_ticket ticket = {0};
    const enum leme_public_status status = timao_client_admit(
        client, TIMAO_CLIENT_WATCH, leme_control_document_value(w->descriptor),
        (struct leme_public_text){0}, w->id, &ticket);
    if (status != LEME_PUBLIC_OK) {
      timao_client_watch_terminate(client, w, TIMAO_CLIENT_RESOURCE);
      continue;
    }
    w->pending_registration = true;
    ++occupied;
  }
  bool complete = client->helper_state == TIMAO_HELPER_NONE;
  for (size_t i = 0; i < 32; ++i) {
    const struct timao_client_watch_slot *w = &client->watches[i];
    if (w->id != 0 && w->interest && !w->canceled && w->lost)
      complete = false;
  }
  if (complete) {
    client->recovering = false;
    client->retry_step = 0;
  }
}

enum leme_public_status
timao_client_helper_reply(struct timao_client *client,
                          const struct leme_public_value *root, bool ok) {
  if (!ok || leme_public_kind(leme_public_get(
                 root, LEME_PUBLIC_TEXT("revision"))) != LEME_PUBLIC_NULL)
    return LEME_PUBLIC_INVALID;
  const struct leme_public_value *value =
      leme_public_get(root, LEME_PUBLIC_TEXT("value"));
  if (client->helper_state == TIMAO_HELPER_REMOVING) {
    if (leme_public_kind(value) != LEME_PUBLIC_NULL)
      return LEME_PUBLIC_INVALID;
    client->helper_state = TIMAO_HELPER_NONE;
    client->helper_length = 0;
    client->helper_sequence = 0;
  } else if (client->helper_state == TIMAO_HELPER_REGISTERING) {
    struct leme_public_text subscription = {0};
    if (leme_public_as_text(
            leme_public_get(value, LEME_PUBLIC_TEXT("subscription")),
            &subscription) != LEME_PUBLIC_OK ||
        subscription.length == 0 || subscription.length > 128 ||
        memchr(subscription.data, '\0', subscription.length) != NULL)
      return LEME_PUBLIC_INVALID;
    for (size_t i = 0; i < 32; ++i) {
      const struct timao_client_watch_slot *w = &client->watches[i];
      if (w->subscription_length == subscription.length &&
          memcmp(w->subscription, subscription.data, subscription.length) == 0)
        return LEME_PUBLIC_INVALID;
    }
    memcpy(client->helper_subscription, subscription.data, subscription.length);
    client->helper_subscription[subscription.length] = '\0';
    client->helper_length = subscription.length;
    client->helper_state = TIMAO_HELPER_ACTIVE;
  } else {
    return LEME_PUBLIC_INVALID;
  }
  client->helper_ticket = (struct timao_client_ticket){0};
  return LEME_PUBLIC_OK;
}

enum leme_public_status
timao_client_helper_event(struct timao_client *client,
                          const struct leme_public_value *event) {
  uint64_t sequence = 0;
  if (leme_public_kind(leme_public_get(event, LEME_PUBLIC_TEXT("revision"))) !=
          LEME_PUBLIC_NULL ||
      !timao_client_decimal(
          leme_public_get(event, LEME_PUBLIC_TEXT("sequence")), &sequence) ||
      sequence <= client->helper_sequence || sequence == UINT64_MAX)
    return LEME_PUBLIC_INVALID;
  const enum leme_public_status status = timao_client_recovery_status(
      client, leme_public_get(event, LEME_PUBLIC_TEXT("value")));
  if (status == LEME_PUBLIC_OK)
    client->helper_sequence = sequence;
  return status;
}

void timao_client_shutdown_tick(struct timao_client *client) {
  if (!client->shutting_down || client->state != TIMAO_CLIENT_READY)
    return;
  if (client->clock.now_ms(client->clock.context) >= client->shutdown_due) {
    timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
    return;
  }
  timao_client_watches_pump(client);
  if (client->helper_state == TIMAO_HELPER_ACTIVE &&
      client->pending_count < client->remote.outstanding) {
    if (timao_client_admit(
            client, TIMAO_CLIENT_UNWATCH, NULL,
            (struct leme_public_text){client->helper_subscription,
                                      client->helper_length},
            0, &client->helper_ticket) != LEME_PUBLIC_OK) {
      timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
      return;
    }
    client->helper_state = TIMAO_HELPER_REMOVING;
  }
  if (client->watch_count == 0 && client->helper_state == TIMAO_HELPER_NONE &&
      client->pending_count == 0)
    timao_client_fail(client, TIMAO_CLIENT_CANCELLED);
}
