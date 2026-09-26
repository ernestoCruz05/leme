#ifndef TIMAO_CLIENT_CODEC_H
#define TIMAO_CLIENT_CODEC_H

#include "control/decode.h"
#include "timao/client.h"

bool timao_client_decimal(const struct leme_public_value *value, uint64_t *out);

enum leme_public_status
timao_client_limits_resolve(const struct timao_client_limits *limits,
                            struct timao_client_limits *out);

enum leme_public_status timao_client_record_decode(
    struct leme_public_budget *account, struct leme_public_text bytes,
    const struct timao_client_limits *limits,
    struct leme_control_document **out, struct leme_control_error *error);

#endif
