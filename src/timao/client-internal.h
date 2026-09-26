#ifndef TIMAO_CLIENT_INTERNAL_H
#define TIMAO_CLIENT_INTERNAL_H

#include "timao/client-codec.h"
#include "ipc/frame.h"

enum timao_client_state {
  TIMAO_CLIENT_IDLE,
  TIMAO_CLIENT_CONNECTING,
  TIMAO_CLIENT_NEGOTIATING,
  TIMAO_CLIENT_READY,
  TIMAO_CLIENT_CLOSING
};

struct timao_client_message {
  struct timao_client_message *next;
  struct leme_control_document *document;
  enum timao_client_message_kind kind;
  struct timao_client_ticket ticket;
  struct timao_client_failure failure;
  char request_id[64];
  char *instance_copy;
  uint64_t watch;
  size_t queued_bytes;
};

struct timao_client_pending {
  struct timao_client_message *completion;
  struct leme_control_frame *frame;
  size_t offset, length;
  uint64_t deadline;
  bool offered, internal;
};

struct timao_client_watch_slot {
  uint64_t id, generation, sequence;
  struct leme_control_document *descriptor;
  struct timao_client_message *terminal;
  char subscription[129];
  size_t subscription_length;
  bool pending_registration, pending_unwatch, canceled, sensitive;
  bool interest, acknowledged, suspended, lost;
  const char *reset_reason;
};

enum timao_client_helper_state {
  TIMAO_HELPER_NONE,
  TIMAO_HELPER_REGISTERING,
  TIMAO_HELPER_ACTIVE,
  TIMAO_HELPER_REMOVING
};

struct timao_client {
  struct leme_public_budget *account;
  struct timao_client_limits limits;
  struct timao_client_clock clock;
  enum timao_client_state state;
  uint64_t epoch, serial, deadline;
  struct timao_client_message *emergency;
  struct timao_client_message *head, *tail;
  struct timao_client_pending pending[16];
  size_t pending_count;
  struct leme_control_framer *framer;
  struct leme_control_document *hello;
  struct leme_public_text instance;
  struct leme_control_limits remote;
  bool query_capability, action_capability, watch_capability;
  struct timao_client_watch_slot watches[32];
  uint64_t next_watch;
  size_t watch_count, queued_bytes;
  bool retry_pending, recovering, shutting_down;
  unsigned retry_step;
  uint64_t retry_due, shutdown_due;
  uint32_t random_state;
  bool recovery_status_known, recovery_locked;
  struct timao_client_ticket status_ticket, helper_ticket;
  enum timao_client_helper_state helper_state;
  char helper_subscription[129];
  size_t helper_length;
  uint64_t helper_sequence;
};

uint64_t timao_client_deadline(uint64_t now, uint64_t delay);
enum leme_public_status timao_client_allocation_status(void);
size_t
timao_client_message_metadata(const struct timao_client_message *message);
size_t timao_client_delivery_arena(const struct timao_client *client,
                                   size_t metadata);
bool timao_client_deliver(struct timao_client *client,
                          struct timao_client_message *message);
void timao_client_enqueue(struct timao_client *client,
                          struct timao_client_message *message);
void timao_client_transport_notice(struct timao_client *client,
                                   struct timao_client_message *message);
void timao_client_fail(struct timao_client *client,
                       enum timao_client_cause cause);
void timao_client_requests_clear(struct timao_client *client);
void timao_client_requests_fail(struct timao_client *client,
                                enum timao_client_cause cause);
void timao_client_requests_tick(struct timao_client *client, uint64_t now);

enum leme_public_status
timao_client_admit(struct timao_client *client,
                   enum timao_client_operation operation,
                   const struct leme_public_value *expression,
                   struct leme_public_text subscription, uint64_t watch,
                   struct timao_client_ticket *ticket);
bool timao_client_watch_canceled(struct timao_client *client, uint64_t watch);
void timao_client_watches_clear(struct timao_client *client);
void timao_client_watches_lost(struct timao_client *client,
                               enum timao_client_cause cause);
void timao_client_watches_pump(struct timao_client *client);
enum leme_public_status
timao_client_watch_reply(struct timao_client *client,
                         struct timao_client_pending *pending,
                         struct leme_control_document **doc, bool ok);
enum leme_public_status
timao_client_watch_event(struct timao_client *client,
                         const struct leme_public_value *event);

bool timao_client_recovery_interested(const struct timao_client *client);
void timao_client_recovery_lost(struct timao_client *client,
                                enum timao_client_cause cause);
void timao_client_recovery_tick(struct timao_client *client);
void timao_client_recovery_pump(struct timao_client *client);
void timao_client_shutdown_tick(struct timao_client *client);
enum leme_public_status
timao_client_recovery_status(struct timao_client *client,
                             const struct leme_public_value *value);
enum leme_public_status
timao_client_helper_reply(struct timao_client *client,
                          const struct leme_public_value *root, bool ok);
enum leme_public_status
timao_client_helper_event(struct timao_client *client,
                          const struct leme_public_value *event);
void timao_client_watch_terminate(struct timao_client *client,
                                  struct timao_client_watch_slot *watch,
                                  enum timao_client_cause cause);

#endif
