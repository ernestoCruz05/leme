#ifndef LEME_CONTROL_REPLY_H
#define LEME_CONTROL_REPLY_H

#include "control/error.h"
#include "control/limits.h"
#include "ipc/frame.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_json;
bool leme_control_error_write_json(struct leme_json *json,
                                   const struct leme_control_error *error);

enum leme_control_code
leme_control_reply_create_value(size_t max_response_bytes, const char *id,
                                size_t id_len, const char *instance,
                                size_t instance_len,
                                const struct leme_public_value *value,
                                struct leme_control_frame **out_frame);

enum leme_control_code
leme_control_reply_create_error(size_t max_response_bytes, const char *id,
                                size_t id_len, const char *instance,
                                size_t instance_len,
                                const struct leme_control_error *error,
                                struct leme_control_frame **out_frame);

#endif
