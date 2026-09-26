#ifndef LEME_CONTROL_REQUEST_H
#define LEME_CONTROL_REQUEST_H

#include "control/decode.h"
#include "control/error.h"
#include "public/value.h"

enum leme_control_request_op {
  LEME_CONTROL_HELLO,
  LEME_CONTROL_QUERY,
  LEME_CONTROL_ACT,
  LEME_CONTROL_WATCH,
  LEME_CONTROL_UNWATCH
};

struct leme_control_request;

enum leme_control_code
leme_control_request_create(struct leme_control_document **document,
                            struct leme_control_request **out,
                            struct leme_control_error *error);
void leme_control_request_ref(struct leme_control_request *request);
void leme_control_request_destroy(struct leme_control_request *request);
struct leme_public_text
leme_control_request_id(const struct leme_control_request *request);
struct leme_public_text
leme_control_request_instance(const struct leme_control_request *request);
struct leme_public_text
leme_control_request_subscription(const struct leme_control_request *request);
const struct leme_public_value *
leme_control_request_expression(const struct leme_control_request *request);
enum leme_control_request_op
leme_control_request_operation(const struct leme_control_request *request);

#endif
