#ifndef TIMAO_CLIENT_H
#define TIMAO_CLIENT_H

#include "public/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct timao_client;
struct timao_client_message;
struct timao_client_clock {
  void *context;
  uint64_t (*now_ms)(void *);
  uint32_t (*random_u32)(void *);
};
struct timao_client_limits {
  size_t total_bytes, queued_bytes, watches;
  uint64_t handshake_ms, request_ms, shutdown_ms;
};
struct timao_client_ticket {
  uint64_t epoch, serial;
};
enum timao_client_operation {
  TIMAO_CLIENT_HELLO,
  TIMAO_CLIENT_QUERY,
  TIMAO_CLIENT_ACT,
  TIMAO_CLIENT_WATCH,
  TIMAO_CLIENT_UNWATCH
};
enum timao_client_cause {
  TIMAO_CLIENT_EOF,
  TIMAO_CLIENT_IO_ERROR,
  TIMAO_CLIENT_AMBIGUOUS_IO,
  TIMAO_CLIENT_TIMEOUT,
  TIMAO_CLIENT_PERMISSION,
  TIMAO_CLIENT_PROTOCOL,
  TIMAO_CLIENT_UNSUPPORTED,
  TIMAO_CLIENT_RESOURCE,
  TIMAO_CLIENT_CANCELLED
};
enum timao_client_message_kind {
  TIMAO_CLIENT_REPLY,
  TIMAO_CLIENT_REGISTERED,
  TIMAO_CLIENT_EVENT,
  TIMAO_CLIENT_FAILURE
};
struct timao_client_loss {
  uint64_t epoch;
  enum timao_client_cause cause;
};
struct timao_client_failure {
  enum timao_client_cause cause;
  enum timao_client_operation operation;
  bool outcome_unknown, effects_known_none;
  struct leme_public_text request_id, instance;
};
enum leme_public_status
timao_client_create(struct leme_public_budget *account,
                    const struct timao_client_limits *limits,
                    const struct timao_client_clock *clock,
                    struct timao_client **out);
void timao_client_destroy(struct timao_client *client);
enum leme_public_status timao_client_start(struct timao_client *client);
bool timao_client_wants_connect(const struct timao_client *client);
uint64_t timao_client_connect_epoch(const struct timao_client *client);
bool timao_client_wants_close(const struct timao_client *client);
enum leme_public_status timao_client_opened(struct timao_client *client,
                                            uint64_t epoch);
void timao_client_lost(struct timao_client *client,
                       struct timao_client_loss loss);
void timao_client_closed(struct timao_client *client, uint64_t epoch);
void timao_client_tick(struct timao_client *client);
bool timao_client_ready(const struct timao_client *client);
enum leme_public_status
timao_client_peek_write(struct timao_client *client,
                        struct timao_client_ticket *ticket,
                        struct leme_public_text *bytes);
enum leme_public_status timao_client_written(struct timao_client *client,
                                             struct timao_client_ticket ticket,
                                             size_t accepted_bytes);
enum leme_public_status timao_client_feed(struct timao_client *client,
                                          uint64_t epoch,
                                          struct leme_public_text bytes,
                                          size_t *consumed);
enum leme_public_status
timao_client_request(struct timao_client *client,
                     enum timao_client_operation operation,
                     const struct leme_public_value *expression,
                     struct timao_client_ticket *ticket);
enum leme_public_status
timao_client_watch(struct timao_client *client,
                   const struct leme_public_value *expression, uint64_t *watch,
                   struct timao_client_ticket *ticket);
void timao_client_watch_interest(struct timao_client *client, uint64_t watch,
                                 bool live);
void timao_client_cancel(struct timao_client *client, uint64_t watch);
bool timao_client_request_identity(const struct timao_client *client,
                                   struct timao_client_ticket ticket,
                                   struct leme_public_text *request_id,
                                   struct leme_public_text *instance);
struct timao_client_message *timao_client_take(struct timao_client *client);
struct timao_client_message *timao_client_take_handlers(
    struct timao_client *client, const uint64_t *watches, size_t count);
struct timao_client_message *
timao_client_take_reply(struct timao_client *client,
                        struct timao_client_ticket ticket);
struct timao_client_message *
timao_client_take_watch(struct timao_client *client, uint64_t watch);
enum timao_client_message_kind
timao_client_message_kind(const struct timao_client_message *message);
const struct leme_public_value *
timao_client_message_value(const struct timao_client_message *message);
const struct timao_client_failure *
timao_client_message_failure(const struct timao_client_message *message);
struct timao_client_ticket
timao_client_message_ticket(const struct timao_client_message *message);
uint64_t timao_client_message_watch(const struct timao_client_message *message);
void timao_client_message_destroy(struct timao_client_message *message);
void timao_client_shutdown(struct timao_client *client);

#endif
