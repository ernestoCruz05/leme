#include "public/model-internal.h"
#include "public/value-internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

enum leme_public_status
leme_public_system_entropy(void *context, unsigned char *out, size_t count) {
  (void)context;
  if (out == NULL && count != 0)
    return LEME_PUBLIC_INVALID;
  size_t done = 0;
  while (done < count) {
    const ssize_t got = getrandom(out + done, count - done, GRND_NONBLOCK);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0 || (size_t)got > count - done)
      return LEME_PUBLIC_UNAVAILABLE;
    done += (size_t)got;
  }
  return LEME_PUBLIC_OK;
}

const char *leme_public_entity_name(enum leme_public_entity kind) {
  static const char *const names[] = {"view", "tag", "output", "input"};
  return (unsigned)kind < LEME_PUBLIC_ENTITY_COUNT ? names[(unsigned)kind]
                                                   : NULL;
}

enum leme_public_status
leme_public_model_issue_id(struct leme_public_model *model,
                           struct leme_public_id *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = (struct leme_public_id){0};
  if (!leme_public_model_available(model))
    return LEME_PUBLIC_UNAVAILABLE;
  if (model->serial == UINT64_MAX) {
    leme_public_model_disable(model);
    return LEME_PUBLIC_LIMIT;
  }
  out->serial = ++model->serial;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_model_next_order(struct leme_public_model *model, uint64_t *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = 0;
  if (!leme_public_model_available(model))
    return LEME_PUBLIC_UNAVAILABLE;
  if (model->order >= (uint64_t)LEME_PUBLIC_SAFE_INTEGER) {
    leme_public_model_disable(model);
    return LEME_PUBLIC_LIMIT;
  }
  *out = ++model->order;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_id_value(struct leme_public_builder *b,
                     const struct leme_public_model *model,
                     enum leme_public_entity kind, struct leme_public_id id,
                     uint16_t tag_number, struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (!leme_public_model_available(model))
    return leme_public_fail(b, LEME_PUBLIC_UNAVAILABLE);
  const char *name = leme_public_entity_name(kind);
  if (name == NULL || id.serial == 0 || id.serial > model->serial ||
      (kind == LEME_PUBLIC_TAG ? tag_number == 0 : tag_number != 0))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  char text[57] = {0};
  const int written =
      kind == LEME_PUBLIC_TAG
          ? snprintf(text, sizeof(text), "t:%s:%016" PRIx64 ":%04" PRIx16,
                     model->instance, id.serial, tag_number)
          : snprintf(text, sizeof(text), "%c:%s:%016" PRIx64, name[0],
                     model->instance, id.serial);
  if (written < 0 || (size_t)written >= sizeof(text))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_public_string(
      b, (struct leme_public_text){.data = text, .length = (size_t)written},
      false, out);
}

enum leme_public_status
leme_public_ref_value(struct leme_public_builder *b,
                      const struct leme_public_model *model,
                      enum leme_public_entity kind, struct leme_public_id id,
                      uint16_t tag_number, struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_value *record = NULL, *text = NULL;
  if (leme_public_id_value(b, model, kind, id, tag_number, &text) !=
          LEME_PUBLIC_OK ||
      leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"),
                           leme_public_entity_name(kind)) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("id"), text) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("instance"),
                           leme_public_model_instance(model),
                           false) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static bool hexadecimal(struct leme_public_text text, uint64_t *out) {
  *out = 0;
  for (size_t i = 0; i < text.length; ++i) {
    const unsigned char c = (unsigned char)text.data[i];
    const unsigned value = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                           : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10u
                                                  : 16u;
    if (value == 16u)
      return false;
    *out = (*out << 4u) | value;
  }
  return true;
}

bool leme_public_parse_id(const struct leme_public_model *model,
                          enum leme_public_entity kind,
                          struct leme_public_text text,
                          struct leme_public_id *out, uint16_t *tag_number) {
  if (out == NULL || tag_number == NULL)
    return false;
  *out = (struct leme_public_id){0};
  *tag_number = 0;
  const char *name = leme_public_entity_name(kind);
  const size_t length = kind == LEME_PUBLIC_TAG ? 56u : 51u;
  if (model == NULL || name == NULL || text.data == NULL ||
      text.length != length || text.data[0] != name[0] || text.data[1] != ':' ||
      text.data[34] != ':' || memcmp(text.data + 2, model->instance, 32) != 0)
    return false;
  uint64_t serial = 0, slot = 0;
  if (!hexadecimal(
          (struct leme_public_text){.data = text.data + 35, .length = 16},
          &serial) ||
      serial == 0 || serial > model->serial)
    return false;
  if (kind == LEME_PUBLIC_TAG &&
      (text.data[51] != ':' ||
       !hexadecimal(
           (struct leme_public_text){.data = text.data + 52, .length = 4},
           &slot) ||
       slot == 0))
    return false;
  out->serial = serial;
  *tag_number = (uint16_t)slot;
  return true;
}
