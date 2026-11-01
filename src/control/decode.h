#ifndef LEME_CONTROL_DECODE_H
#define LEME_CONTROL_DECODE_H

#include "control/error.h"
#include "control/limits.h"
#include "public/value.h"

struct leme_control_document;

enum leme_control_code leme_control_decode(
    struct leme_public_text bytes, const struct leme_control_limits *limits,
    struct leme_control_document **out, struct leme_control_error *error);
const struct leme_public_value *
leme_control_document_value(const struct leme_control_document *document);
void leme_control_document_destroy(struct leme_control_document *document);

#endif
