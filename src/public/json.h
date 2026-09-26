#ifndef LEME_PUBLIC_JSON_H
#define LEME_PUBLIC_JSON_H

#include "public/value.h"

struct leme_json;
enum leme_public_status
leme_public_write_json(const struct leme_public_value *value,
                       struct leme_json *json);
enum leme_public_status
leme_public_write_json_work(const struct leme_public_value *value,
                            struct leme_json *json,
                            const struct leme_public_work *work);

#endif
