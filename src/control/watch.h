#ifndef LEME_CONTROL_WATCH_H
#define LEME_CONTROL_WATCH_H

#include "control/control.h"
#include "control/request.h"
#include "ipc/frame.h"

struct leme_control_watch_sink {
  void *context;
  enum leme_control_code (*publish)(void *context,
                                    struct leme_control_frame **frames,
                                    size_t count);
  bool (*discard)(void *context, struct leme_control_watch_disposal disposal);
  void (*close)(void *context);
};

struct leme_control_watch_set;

enum leme_control_code
leme_control_watch_set_create(struct leme_public_budget *account,
                              const struct leme_control_limits *limits,
                              const struct leme_control_watch_sink *sink,
                              struct leme_control_watch_set **out);
void leme_control_watch_set_destroy(struct leme_control_watch_set *set);
enum leme_control_code leme_control_watch_request(
    struct leme_control_watch_set *set, struct leme_control_context *context,
    struct leme_control_request *request, struct leme_control_error *error);
void leme_control_watch_notify(struct leme_control_watch_set *set,
                               enum leme_public_change change);
bool leme_control_watch_has_work(const struct leme_control_watch_set *set);
void leme_control_watch_step(struct leme_control_watch_set *set,
                             struct leme_control_context *context);

#endif
