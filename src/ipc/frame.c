
#include "ipc/frame.h"
#include "control/error.h"
#include "control/memory.h"
#include "public/budget.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct leme_control_frame {
  struct leme_public_budget *account;
  size_t length;
  size_t capacity;
  char *bytes;
  enum leme_control_frame_kind kind;
  bool sensitive;
  char id[129];
  size_t id_len;
  uint64_t subscription;
};

struct leme_control_framer {
  struct leme_public_budget *account;
  size_t max_record_bytes;
  char *buffer;
  size_t capacity;
  size_t length;
};

static void *frame_alloc(struct leme_public_budget *account, size_t bytes) {
  if (account != NULL) {
    return leme_control_alloc(account, bytes);
  }
  return malloc(bytes);
}

static void frame_free(struct leme_public_budget *account, void *ptr) {
  if (ptr == NULL) {
    return;
  }
  if (account != NULL) {
    leme_control_free(ptr);
  } else {
    free(ptr);
  }
}

static enum leme_control_code allocation_error(void) {
  return errno == ENOSPC || errno == EOVERFLOW ? LEME_CONTROL_RESOURCE_LIMIT
                                               : LEME_CONTROL_OUT_OF_MEMORY;
}

enum leme_control_code
leme_control_frame_create_capacity(struct leme_public_budget *account,
                                   size_t capacity,
                                   struct leme_control_frame **out) {
  if (out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  if (capacity == SIZE_MAX)
    return LEME_CONTROL_RESOURCE_LIMIT;

  struct leme_control_frame *frame = frame_alloc(account, sizeof(*frame));
  if (frame == NULL) {
    return allocation_error();
  }

  size_t alloc_bytes = capacity + 1;
  char *copy = frame_alloc(account, alloc_bytes);
  if (copy == NULL) {
    const enum leme_control_code code = allocation_error();
    frame_free(account, frame);
    return code;
  }
  copy[0] = '\0';

  frame->account = account;
  frame->length = 0;
  frame->capacity = capacity;
  frame->bytes = copy;
  frame->kind = LEME_CONTROL_FRAME_OTHER;
  frame->sensitive = false;
  frame->id[0] = '\0';
  frame->id_len = 0;
  frame->subscription = 0;
  *out = frame;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_control_frame_create(struct leme_public_budget *account, const char *bytes,
                          size_t length, struct leme_control_frame **out) {
  enum leme_control_code code =
      leme_control_frame_create_capacity(account, length, out);
  if (code != LEME_CONTROL_OK) {
    return code;
  }
  if (length > 0 && bytes != NULL) {
    memcpy((*out)->bytes, bytes, length);
  }
  (*out)->bytes[length] = '\0';
  (*out)->length = length;
  return LEME_CONTROL_OK;
}

void leme_control_frame_destroy(struct leme_control_frame *frame) {
  if (frame == NULL) {
    return;
  }
  struct leme_public_budget *account = frame->account;
  frame_free(account, frame->bytes);
  frame_free(account, frame);
}

const char *leme_control_frame_bytes(const struct leme_control_frame *frame) {
  return frame != NULL ? frame->bytes : NULL;
}

char *leme_control_frame_buffer(struct leme_control_frame *frame,
                                size_t *out_capacity) {
  if (frame == NULL) {
    if (out_capacity != NULL) {
      *out_capacity = 0;
    }
    return NULL;
  }
  if (out_capacity != NULL) {
    *out_capacity = frame->capacity;
  }
  return frame->bytes;
}

size_t leme_control_frame_length(const struct leme_control_frame *frame) {
  return frame != NULL ? frame->length : 0;
}

void leme_control_frame_set_length(struct leme_control_frame *frame,
                                   size_t length) {
  if (frame != NULL) {
    frame->length = length;
    if (frame->bytes != NULL && length <= frame->capacity) {
      frame->bytes[length] = '\0';
    }
  }
}

void leme_control_frame_set_metadata(struct leme_control_frame *frame,
                                     enum leme_control_frame_kind kind,
                                     bool sensitive, const char *id,
                                     size_t id_len) {
  if (frame == NULL) {
    return;
  }
  frame->kind = kind;
  frame->sensitive = sensitive;
  if (id != NULL && id_len > 0) {
    size_t copy_len =
        id_len < sizeof(frame->id) - 1 ? id_len : sizeof(frame->id) - 1;
    memcpy(frame->id, id, copy_len);
    frame->id[copy_len] = '\0';
    frame->id_len = copy_len;
  } else {
    frame->id[0] = '\0';
    frame->id_len = 0;
  }
}

void leme_control_frame_set_subscription(struct leme_control_frame *frame,
                                         uint64_t subscription) {
  if (frame != NULL)
    frame->subscription = subscription;
}

uint64_t
leme_control_frame_subscription(const struct leme_control_frame *frame) {
  return frame != NULL ? frame->subscription : 0;
}

enum leme_control_frame_kind
leme_control_frame_get_kind(const struct leme_control_frame *frame) {
  return frame != NULL ? frame->kind : LEME_CONTROL_FRAME_OTHER;
}

bool leme_control_frame_is_sensitive(const struct leme_control_frame *frame) {
  return frame != NULL ? frame->sensitive : false;
}

const char *leme_control_frame_id(const struct leme_control_frame *frame,
                                  size_t *out_len) {
  if (out_len != NULL) {
    *out_len = frame != NULL ? frame->id_len : 0;
  }
  return frame != NULL ? frame->id : NULL;
}

enum leme_control_code
leme_control_framer_create(struct leme_public_budget *account,
                           size_t max_record_bytes,
                           struct leme_control_framer **out) {
  if (out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  struct leme_control_framer *framer = frame_alloc(account, sizeof(*framer));
  if (framer == NULL) {
    return allocation_error();
  }

  framer->account = account;
  framer->max_record_bytes = max_record_bytes;
  framer->buffer = NULL;
  framer->capacity = 0;
  framer->length = 0;
  *out = framer;
  return LEME_CONTROL_OK;
}

void leme_control_framer_destroy(struct leme_control_framer *framer) {
  if (framer == NULL) {
    return;
  }
  struct leme_public_budget *account = framer->account;
  frame_free(account, framer->buffer);
  frame_free(account, framer);
}

size_t
leme_control_framer_pending_bytes(const struct leme_control_framer *framer) {
  return framer != NULL ? framer->length : 0;
}

bool leme_control_framer_is_empty(const struct leme_control_framer *framer) {
  return framer == NULL || framer->length == 0;
}

enum leme_control_code
leme_control_framer_feed(struct leme_control_framer *framer, const char *bytes,
                         size_t length, size_t *consumed,
                         struct leme_control_frame **out) {
  if (framer == NULL || consumed == NULL || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *consumed = 0;
  *out = NULL;

  if (length == 0 || bytes == NULL) {
    return LEME_CONTROL_OK;
  }

  const char *nl = memchr(bytes, '\n', length);
  if (nl != NULL) {
    size_t chunk_len = (size_t)(nl - bytes);
    size_t total_record_len = framer->length + chunk_len;
    if (total_record_len > framer->max_record_bytes) {
      framer->length = 0;
      return LEME_CONTROL_RESOURCE_LIMIT;
    }

    struct leme_control_frame *frame = NULL;
    if (framer->length == 0) {
      enum leme_control_code code =
          leme_control_frame_create(framer->account, bytes, chunk_len, &frame);
      if (code != LEME_CONTROL_OK) {
        return code;
      }
    } else {
      enum leme_control_code code = leme_control_frame_create(
          framer->account, NULL, total_record_len, &frame);
      if (code != LEME_CONTROL_OK) {
        return code;
      }
      if (framer->length > 0) {
        memcpy(frame->bytes, framer->buffer, framer->length);
      }
      if (chunk_len > 0) {
        memcpy(frame->bytes + framer->length, bytes, chunk_len);
      }
      frame->bytes[total_record_len] = '\0';
      framer->length = 0;
    }

    *consumed = chunk_len + 1;
    *out = frame;
    return LEME_CONTROL_OK;
  }

  size_t new_len = framer->length + length;
  if (new_len > framer->max_record_bytes) {
    framer->length = 0;
    return LEME_CONTROL_RESOURCE_LIMIT;
  }

  if (new_len > framer->capacity) {
    size_t new_cap = framer->capacity == 0 ? 128 : framer->capacity * 2;
    while (new_cap < new_len) {
      new_cap *= 2;
    }
    char *new_buf = NULL;
    if (framer->account != NULL) {
      if (framer->buffer != NULL) {
        new_buf = leme_control_realloc(framer->buffer, new_cap);
      } else {
        new_buf = leme_control_alloc(framer->account, new_cap);
      }
    } else {
      new_buf = realloc(framer->buffer, new_cap);
    }
    if (new_buf == NULL) {
      return allocation_error();
    }
    framer->buffer = new_buf;
    framer->capacity = new_cap;
  }

  memcpy(framer->buffer + framer->length, bytes, length);
  framer->length = new_len;
  *consumed = length;
  *out = NULL;
  return LEME_CONTROL_OK;
}
