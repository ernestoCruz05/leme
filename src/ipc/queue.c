#include "ipc/connection-internal.h"
#include "control/memory.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static void *queue_alloc(struct leme_public_budget *account, size_t bytes) {
  if (account != NULL) {
    return leme_control_alloc(account, bytes);
  }
  return malloc(bytes);
}

static void queue_free(struct leme_public_budget *account, void *ptr) {
  if (ptr == NULL) {
    return;
  }
  if (account != NULL) {
    leme_control_free(ptr);
  } else {
    free(ptr);
  }
}

enum leme_control_code
leme_control_queue_push_batch(struct leme_control_queue *queue,
                              struct leme_control_frame **frames,
                              size_t count) {
  if (queue == NULL || frames == NULL || count == 0 || count > 2)
    return LEME_CONTROL_INVALID_ARGUMENT;
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    if (frames[i] == NULL || (i == 1 && frames[i] == frames[0]))
      return LEME_CONTROL_INVALID_ARGUMENT;
    const size_t length = leme_control_frame_length(frames[i]);
    if (length > SIZE_MAX - total)
      return LEME_CONTROL_RESOURCE_LIMIT;
    total += length;
  }
  if (total > queue->max_bytes ||
      queue->queued_bytes > queue->max_bytes - total ||
      queue->reserved_bytes > queue->max_bytes - total - queue->queued_bytes)
    return LEME_CONTROL_RESOURCE_LIMIT;

  struct leme_control_queue_node *nodes[2] = {0};
  enum leme_control_code code = LEME_CONTROL_OK;
  for (size_t i = 0; i < count; ++i) {
    nodes[i] = queue_alloc(queue->account, sizeof(*nodes[i]));
    if (nodes[i] == NULL) {
      code = errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                             : LEME_CONTROL_OUT_OF_MEMORY;
      goto cleanup;
    }
    *nodes[i] = (struct leme_control_queue_node){.frame = frames[i]};
  }
  for (size_t i = 1; i < count; ++i)
    nodes[i - 1]->next = nodes[i];
  if (queue->tail != NULL)
    queue->tail->next = nodes[0];
  else
    queue->head = nodes[0];
  queue->tail = nodes[count - 1];
  queue->queued_bytes += total;
  for (size_t i = 0; i < count; ++i)
    frames[i] = NULL;
  return LEME_CONTROL_OK;

cleanup:
  for (size_t i = 0; i < count; ++i)
    queue_free(queue->account, nodes[i]);
  return code;
}

bool leme_control_queue_discard_watch(
    struct leme_control_queue *queue,
    const struct leme_control_watch_disposal disposal) {
  const uint64_t subscription = disposal.subscription;
  const enum leme_control_watch_discard reason = disposal.reason;
  if (queue == NULL || (reason != LEME_WATCH_DISCARD_CANCEL &&
                        reason != LEME_WATCH_DISCARD_LOCK))
    return false;
  if (reason == LEME_WATCH_DISCARD_LOCK) {
    for (const struct leme_control_queue_node *node = queue->head; node != NULL;
         node = node->next) {
      if (leme_control_frame_is_sensitive(node->frame) &&
          (node->offset != 0 || leme_control_frame_get_kind(node->frame) !=
                                    LEME_CONTROL_FRAME_EVENT))
        return false;
    }
  }

  struct leme_control_queue_node *previous = NULL;
  struct leme_control_queue_node *node = queue->head;
  while (node != NULL) {
    struct leme_control_queue_node *next = node->next;
    const enum leme_control_frame_kind kind =
        leme_control_frame_get_kind(node->frame);
    const bool event = kind == LEME_CONTROL_FRAME_EVENT ||
                       kind == LEME_CONTROL_FRAME_TERMINAL_EVENT;
    const uint64_t token = leme_control_frame_subscription(node->frame);
    const bool matching = subscription != 0 && token == subscription;
    const bool removable = reason == LEME_WATCH_DISCARD_CANCEL ||
                           leme_control_frame_is_sensitive(node->frame);
    if (matching && event && removable && node->offset == 0) {
      if (previous == NULL)
        queue->head = next;
      else
        previous->next = next;
      if (queue->tail == node)
        queue->tail = previous;
      queue->queued_bytes -= leme_control_frame_length(node->frame);
      leme_control_frame_destroy(node->frame);
      queue_free(queue->account, node);
    } else {
      previous = node;
    }
    node = next;
  }
  return true;
}

enum leme_control_code
leme_control_queue_create(struct leme_public_budget *account, size_t max_bytes,
                          struct leme_control_queue **out) {
  if (out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  struct leme_control_queue *queue = queue_alloc(account, sizeof(*queue));
  if (queue == NULL) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  memset(queue, 0, sizeof(*queue));

  queue->account = account;
  queue->max_bytes = max_bytes;
  queue->queued_bytes = 0;
  queue->reserved_bytes = 0;
  queue->reserved_node = NULL;
  queue->head = NULL;
  queue->tail = NULL;
  queue->completion_cb = NULL;
  queue->completion_data = NULL;
  *out = queue;
  return LEME_CONTROL_OK;
}

void leme_control_queue_destroy(struct leme_control_queue *queue) {
  if (queue == NULL) {
    return;
  }
  struct leme_public_budget *account = queue->account;
  if (queue->reserved_node != NULL) {
    queue_free(account, queue->reserved_node);
    queue->reserved_node = NULL;
  }
  struct leme_control_queue_node *cur = queue->head;
  while (cur != NULL) {
    struct leme_control_queue_node *next = cur->next;
    if (queue->completion_cb != NULL) {
      queue->completion_cb(cur->frame, queue->completion_data);
    }
    leme_control_frame_destroy(cur->frame);
    queue_free(account, cur);
    cur = next;
  }
  queue_free(account, queue);
}

void leme_control_queue_set_completion(
    struct leme_control_queue *queue, leme_control_queue_completion_fn callback,
    void *user_data) {
  if (queue == NULL) {
    return;
  }
  queue->completion_cb = callback;
  queue->completion_data = user_data;
}

enum leme_control_code
leme_control_queue_reserve(struct leme_control_queue *queue, size_t bytes) {
  if (queue == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  if (bytes > queue->max_bytes ||
      queue->queued_bytes > queue->max_bytes - bytes ||
      queue->reserved_bytes > queue->max_bytes - bytes - queue->queued_bytes) {
    return LEME_CONTROL_RESOURCE_LIMIT;
  }
  if (queue->reserved_node == NULL) {
    queue->reserved_node =
        queue_alloc(queue->account, sizeof(*queue->reserved_node));
    if (queue->reserved_node == NULL) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }
  queue->reserved_bytes += bytes;
  return LEME_CONTROL_OK;
}

void leme_control_queue_cancel_reservation(struct leme_control_queue *queue,
                                           size_t bytes) {
  if (queue == NULL) {
    return;
  }
  if (bytes >= queue->reserved_bytes) {
    queue->reserved_bytes = 0;
  } else {
    queue->reserved_bytes -= bytes;
  }
  if (queue->reserved_bytes == 0 && queue->reserved_node != NULL) {
    queue_free(queue->account, queue->reserved_node);
    queue->reserved_node = NULL;
  }
}

enum leme_control_code
leme_control_queue_push(struct leme_control_queue *queue,
                        struct leme_control_frame *frame) {
  if (queue == NULL || frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  size_t flen = leme_control_frame_length(frame);
  if (flen > queue->max_bytes ||
      queue->queued_bytes > queue->max_bytes - flen ||
      queue->reserved_bytes > queue->max_bytes - flen - queue->queued_bytes) {
    return LEME_CONTROL_RESOURCE_LIMIT;
  }

  struct leme_control_queue_node *node =
      queue_alloc(queue->account, sizeof(*node));
  if (node == NULL) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  node->frame = frame;
  node->offset = 0;
  node->next = NULL;

  if (queue->tail != NULL) {
    queue->tail->next = node;
    queue->tail = node;
  } else {
    queue->head = node;
    queue->tail = node;
  }
  queue->queued_bytes += flen;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_control_queue_push_reserved(struct leme_control_queue *queue,
                                 struct leme_control_frame *frame) {
  if (queue == NULL || frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  size_t flen = leme_control_frame_length(frame);

  struct leme_control_queue_node *node = queue->reserved_node;
  if (node != NULL) {
    queue->reserved_node = NULL;
  } else {
    node = queue_alloc(queue->account, sizeof(*node));
    if (node == NULL) {
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
  }

  if (flen >= queue->reserved_bytes) {
    queue->reserved_bytes = 0;
  } else {
    queue->reserved_bytes -= flen;
  }

  node->frame = frame;
  node->offset = 0;
  node->next = NULL;

  if (queue->tail != NULL) {
    queue->tail->next = node;
    queue->tail = node;
  } else {
    queue->head = node;
    queue->tail = node;
  }
  queue->queued_bytes += flen;
  return LEME_CONTROL_OK;
}

enum leme_control_flush_status
leme_control_queue_flush(struct leme_control_queue *queue, int fd,
                         size_t *written) {
  if (written != NULL) {
    *written = 0;
  }
  if (queue == NULL) {
    return LEME_CONTROL_FLUSH_ERROR;
  }

  while (queue->head != NULL) {
    struct leme_control_queue_node *node = queue->head;
    size_t flen = leme_control_frame_length(node->frame);
    size_t to_write = flen - node->offset;
    const char *data = leme_control_frame_bytes(node->frame) + node->offset;

    ssize_t ret = send(fd, data, to_write, MSG_NOSIGNAL);
    if (ret > 0) {
      size_t n = (size_t)ret;
      node->offset += n;
      queue->queued_bytes -= n;
      if (written != NULL) {
        *written += n;
      }
      if (node->offset >= flen) {
        queue->head = node->next;
        if (queue->head == NULL) {
          queue->tail = NULL;
        }
        if (queue->completion_cb != NULL) {
          queue->completion_cb(node->frame, queue->completion_data);
        }
        leme_control_frame_destroy(node->frame);
        queue_free(queue->account, node);
      }
    } else if (ret == 0) {
      return LEME_CONTROL_FLUSH_CLOSED;
    } else {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return LEME_CONTROL_FLUSH_AGAIN;
      }
      if (errno == EPIPE || errno == ECONNRESET) {
        return LEME_CONTROL_FLUSH_CLOSED;
      }
      return LEME_CONTROL_FLUSH_ERROR;
    }
  }

  return LEME_CONTROL_FLUSH_OK;
}

size_t leme_control_queue_bytes(const struct leme_control_queue *queue) {
  return queue != NULL ? (queue->queued_bytes + queue->reserved_bytes) : 0;
}

bool leme_control_queue_is_empty(const struct leme_control_queue *queue) {
  return queue == NULL || queue->head == NULL;
}
