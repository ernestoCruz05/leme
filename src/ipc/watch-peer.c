#include "ipc/watch-peer.h"
#include "ipc/connection-internal.h"
#include "control/reply.h"
#include "control/watch.h"

#include <string.h>

static enum leme_control_code
publish(void *context, struct leme_control_frame **frames, size_t count) {
  struct leme_control_peer *peer = context;
  if (peer->state == LEME_PEER_STATE_CLOSED || count == 0 || count > 2)
    return LEME_CONTROL_RESOURCE_LIMIT;
  struct leme_control_outstanding *outstanding = NULL;
  struct leme_control_frame *previous = NULL;
  for (size_t i = 0; i < count; ++i) {
    size_t length = 0;
    const char *id = leme_control_frame_id(frames[i], &length);
    if (length == 0)
      continue;
    if (i != 0 || outstanding != NULL || peer->watch_reply_slot < 0 ||
        peer->watch_reply_slot >= 16)
      return LEME_CONTROL_INVALID_ARGUMENT;
    outstanding = &peer->outstanding[(size_t)peer->watch_reply_slot];
    if (!outstanding->active || outstanding->id_len != length ||
        memcmp(outstanding->id, id, length) != 0)
      return LEME_CONTROL_INVALID_ARGUMENT;
    previous = outstanding->frame;
  }
  if (outstanding != NULL)
    outstanding->frame = frames[0];
  const enum leme_control_code code =
      leme_control_queue_push_batch(peer->queue, frames, count);
  if (code != LEME_CONTROL_OK && outstanding != NULL)
    outstanding->frame = previous;
  return code;
}

static bool discard(void *context,
                    struct leme_control_watch_disposal disposal) {
  struct leme_control_peer *peer = context;
  return leme_control_queue_discard_watch(peer->queue, disposal);
}

static void close_peer(void *context) { leme_control_peer_close(context); }

void leme_control_watch_peer_notify(struct leme_control_peer *peer,
                                    enum leme_public_change change) {
  if (peer == NULL || peer->state == LEME_PEER_STATE_CLOSED)
    return;
  if (change == LEME_PUBLIC_LOCKED_CHANGED &&
      !leme_control_queue_discard_watch(
          peer->queue, (struct leme_control_watch_disposal){
                           .reason = LEME_WATCH_DISCARD_LOCK})) {
    leme_control_peer_close(peer);
    return;
  }
  leme_control_watch_notify(peer->watches, change);
}

enum leme_control_code
leme_control_watch_peer_open(struct leme_control_peer *peer) {
  if (peer == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  if (peer->watches != NULL)
    return LEME_CONTROL_OK;
  peer->watch_reply_slot = -1;
  const struct leme_control_watch_sink sink = {.context = peer,
                                               .publish = publish,
                                               .discard = discard,
                                               .close = close_peer};
  return leme_control_watch_set_create(peer->account, &peer->limits, &sink,
                                       &peer->watches);
}

enum leme_control_code
leme_control_watch_peer_request(struct leme_control_peer *peer,
                                struct leme_control_request *request,
                                int slot) {
  if (peer == NULL || request == NULL || slot < 0 || slot >= 16)
    return LEME_CONTROL_INVALID_ARGUMENT;
  struct leme_control_frame *error_reply = NULL;
  struct leme_control_context *context = NULL;
  struct leme_control_error error = {0};
  enum leme_control_code code = leme_control_watch_peer_open(peer);
  if (code == LEME_CONTROL_OK)
    code = leme_control_context_create(
        leme_control_context_server(peer->context),
        leme_control_context_model(peer->context),
        leme_control_context_source(peer->context), peer->account,
        &peer->limits, leme_control_context_domain(peer->context), &context);
  if (code == LEME_CONTROL_OK) {
    leme_control_context_set_deadline(
        context, leme_control_context_deadline(peer->context));
    peer->watch_reply_slot = slot;
    code = leme_control_watch_request(peer->watches, context, request, &error);
    peer->watch_reply_slot = -1;
  }
  leme_control_context_destroy(context);
  if (code == LEME_CONTROL_OK || peer->state == LEME_PEER_STATE_CLOSED)
    return code;
  if (error.code == LEME_CONTROL_OK)
    error = (struct leme_control_error){.code = code,
                                        .phase = LEME_CONTROL_PREFLIGHT,
                                        .message = "watch request failed",
                                        .expr_path = "/expr"};
  const struct leme_public_text id = leme_control_request_id(request);
  const struct leme_public_text instance =
      leme_public_model_instance(leme_control_context_model(peer->context));
  enum leme_control_code reply_code = leme_control_reply_create_error(
      peer->account, peer->limits.response_bytes, id.data, id.length,
      instance.data, instance.length, NULL, 0, &error, &error_reply);
  if (reply_code == LEME_CONTROL_OK) {
    leme_control_frame_set_metadata(error_reply, LEME_CONTROL_FRAME_QUERY,
                                    error.sensitive, id.data, id.length);
    peer->watch_reply_slot = slot;
    reply_code = publish(peer, &error_reply, 1);
    peer->watch_reply_slot = -1;
  }
  leme_control_frame_destroy(error_reply);
  if (reply_code != LEME_CONTROL_OK)
    leme_control_peer_close(peer);
  return reply_code;
}

void leme_control_watch_peer_step(struct leme_control_peer *peer) {
  struct leme_control_context *context = NULL;
  const enum leme_control_code code = leme_control_context_create(
      leme_control_context_server(peer->context),
      leme_control_context_model(peer->context),
      leme_control_context_source(peer->context), peer->account, &peer->limits,
      leme_control_context_domain(peer->context), &context);
  if (code != LEME_CONTROL_OK) {
    leme_control_peer_close(peer);
    return;
  }
  leme_control_context_set_deadline(
      context, leme_control_context_deadline(peer->context));
  leme_control_watch_step(peer->watches, context);
  leme_control_context_destroy(context);
}
