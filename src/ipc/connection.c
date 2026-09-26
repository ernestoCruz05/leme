#include "ipc/connection.h"
#include "control/control.h"
#include "control/decode.h"
#include "control/dispatch.h"
#include "control/error.h"
#include "control/limits.h"
#include "control/reply.h"
#include "control/request.h"
#include "control/watch.h"
#include "ipc/watch-peer.h"
#include "ipc/connection-internal.h"
#include "ipc/frame.h"
#include "ipc/schedule.h"
#include "public/budget.h"
#include "public/model.h"
#include "public/value.h"

#include "control/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct leme_control_pending_req {
  struct leme_control_request *req;
  struct leme_control_frame *preformed_frame;
  int slot;
  bool fatal;
  struct wl_list link;
};

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

static struct leme_control_pending_req *
pending_req_create(struct leme_control_peer *peer) {
  if (peer == NULL) {
    return NULL;
  }
  struct leme_control_pending_req *preq =
      peer->account != NULL ? leme_control_alloc(peer->account, sizeof(*preq))
                            : malloc(sizeof(*preq));
  if (preq != NULL) {
    memset(preq, 0, sizeof(*preq));
    wl_list_init(&preq->link);
  }
  return preq;
}

static void pending_req_destroy(struct leme_control_peer *peer,
                                struct leme_control_pending_req *preq) {
  if (preq == NULL) {
    return;
  }
  if (preq->req != NULL) {
    leme_control_request_destroy(preq->req);
    preq->req = NULL;
  }
  if (preq->preformed_frame != NULL) {
    leme_control_frame_destroy(preq->preformed_frame);
    preq->preformed_frame = NULL;
  }
  if (peer != NULL && peer->account != NULL) {
    leme_control_free(preq);
  } else {
    free(preq);
  }
}

static void on_peer_frame_complete(struct leme_control_frame *frame,
                                   void *user_data) {
  struct leme_control_peer *peer = user_data;
  if (peer == NULL || frame == NULL) {
    return;
  }
  for (size_t i = 0; i < 16; i++) {
    if (peer->outstanding[i].active && peer->outstanding[i].frame == frame) {
      peer->outstanding[i].active = false;
      peer->outstanding[i].frame = NULL;
      peer->outstanding[i].id_len = 0;
      peer->outstanding[i].id[0] = '\0';
      if (peer->outstanding_count > 0) {
        peer->outstanding_count--;
      }
      break;
    }
  }
}

static bool peer_has_outstanding_id(const struct leme_control_peer *peer,
                                    const char *id, size_t id_len) {
  for (size_t i = 0; i < 16; i++) {
    if (peer->outstanding[i].active && peer->outstanding[i].id_len == id_len &&
        memcmp(peer->outstanding[i].id, id, id_len) == 0) {
      return true;
    }
  }
  return false;
}

static int peer_add_outstanding(struct leme_control_peer *peer, const char *id,
                                size_t id_len) {
  if (peer->outstanding_count >= 16 || id_len > 128) {
    return -1;
  }
  for (size_t i = 0; i < 16; i++) {
    if (!peer->outstanding[i].active) {
      peer->outstanding[i].active = true;
      memcpy(peer->outstanding[i].id, id, id_len);
      peer->outstanding[i].id[id_len] = '\0';
      peer->outstanding[i].id_len = id_len;
      peer->outstanding[i].frame = NULL;
      peer->outstanding_count++;
      return (int)i;
    }
  }
  return -1;
}

static void peer_close_socket(struct leme_control_peer *peer) {
  if (peer == NULL || peer->state == LEME_PEER_STATE_CLOSED)
    return;
  peer->state = LEME_PEER_STATE_CLOSED;
  leme_control_watch_notify(peer->watches, LEME_PUBLIC_DISABLED);
  if (peer->source != NULL) {
    wl_event_source_remove(peer->source);
    peer->source = NULL;
  }
  if (peer->in_runnable) {
    wl_list_remove(&peer->runnable_link);
    wl_list_init(&peer->runnable_link);
    peer->in_runnable = false;
  }
  if (peer->fd >= 0) {
    close(peer->fd);
    peer->fd = -1;
  }
}

void leme_control_peer_close(struct leme_control_peer *peer) {
  peer_close_socket(peer);
}

static void leme_control_peer_flush(struct leme_control_peer *peer) {
  if (peer->fd < 0 || peer->state == LEME_PEER_STATE_CLOSED) {
    return;
  }
  const struct leme_public_source *source =
      leme_control_context_source(peer->context);
  const bool is_locked = source != NULL && source->locked != NULL &&
                         source->locked(source->context);
  if (source != NULL && source->locked != NULL)
    leme_public_model_lock_changed(leme_control_context_model(peer->context),
                                   is_locked);
  if (peer->state == LEME_PEER_STATE_CLOSED)
    return;
  if (is_locked && !leme_control_queue_discard_watch(
                       peer->queue, (struct leme_control_watch_disposal){
                                        .reason = LEME_WATCH_DISCARD_LOCK})) {
    peer_close_socket(peer);
    return;
  }

  size_t written = 0;
  enum leme_control_flush_status status =
      leme_control_queue_flush(peer->queue, peer->fd, &written);
  if (status == LEME_CONTROL_FLUSH_AGAIN) {
    if (peer->source != NULL) {
      wl_event_source_fd_update(peer->source,
                                WL_EVENT_READABLE | WL_EVENT_WRITABLE);
    }
  } else {
    if (peer->source != NULL) {
      wl_event_source_fd_update(peer->source, WL_EVENT_READABLE);
    }
    if (status == LEME_CONTROL_FLUSH_CLOSED ||
        status == LEME_CONTROL_FLUSH_ERROR ||
        (peer->state == LEME_PEER_STATE_DRAINING &&
         wl_list_empty(&peer->pending_requests) &&
         leme_control_queue_is_empty(peer->queue))) {
      peer_close_socket(peer);
    }
  }
}

static enum leme_control_code
dispatch_peer_request(struct leme_control_peer *peer,
                      const struct leme_control_request *request,
                      struct leme_control_frame **out) {
  struct leme_control_context *context = NULL;
  enum leme_control_code code = leme_control_context_create(
      leme_control_context_server(peer->context),
      leme_control_context_model(peer->context),
      leme_control_context_source(peer->context), peer->account, &peer->limits,
      leme_control_context_domain(peer->context), &context);
  if (code != LEME_CONTROL_OK) {
    return code;
  }
  leme_control_context_set_deadline(
      context, leme_control_context_deadline(peer->context));
  code = leme_control_dispatch_request(context, peer, request, out);
  leme_control_context_destroy(context);
  return code;
}

static bool watch_job(const struct leme_control_peer *peer) {
  return peer->state == LEME_PEER_STATE_READY &&
         leme_control_watch_has_work(peer->watches) &&
         (peer->watch_next || wl_list_empty(&peer->pending_requests));
}

bool leme_control_peer_needs_fresh_turn(const struct leme_control_peer *peer) {
  if (peer == NULL || peer->state == LEME_PEER_STATE_CLOSED)
    return false;
  if (watch_job(peer))
    return true;
  if (wl_list_empty(&peer->pending_requests))
    return false;
  const struct leme_control_pending_req *pending =
      wl_container_of(peer->pending_requests.next, pending, link);
  if (!peer->negotiated || pending->req == NULL ||
      pending->preformed_frame != NULL ||
      leme_control_request_operation(pending->req) != LEME_CONTROL_WATCH)
    return false;
  const struct leme_public_text instance =
      leme_control_request_instance(pending->req);
  const struct leme_public_text current =
      leme_public_model_instance(leme_control_context_model(peer->context));
  return instance.length == current.length &&
         memcmp(instance.data, current.data, instance.length) == 0;
}

void leme_control_peer_step(struct leme_control_peer *peer) {
  if (peer == NULL || peer->state == LEME_PEER_STATE_CLOSED) {
    return;
  }

  if (watch_job(peer)) {
    peer->watch_next = false;
    leme_control_watch_peer_step(peer);
    leme_control_peer_flush(peer);
    return;
  }
  if (!wl_list_empty(&peer->pending_requests)) {
    peer->watch_next = true;
    struct leme_control_pending_req *preq =
        wl_container_of(peer->pending_requests.next, preq, link);
    wl_list_remove(&preq->link);
    wl_list_init(&preq->link);

    if (preq->preformed_frame != NULL) {
      enum leme_control_code qcode =
          leme_control_queue_push(peer->queue, preq->preformed_frame);
      if (qcode != LEME_CONTROL_OK) {
        leme_control_frame_destroy(preq->preformed_frame);
        preq->preformed_frame = NULL;
        pending_req_destroy(peer, preq);
        peer_close_socket(peer);
        return;
      }
      preq->preformed_frame = NULL;
      if (preq->fatal) {
        peer->state = LEME_PEER_STATE_DRAINING;
      }
      pending_req_destroy(peer, preq);
      leme_control_peer_flush(peer);
      return;
    }

    struct leme_public_text req_id = leme_control_request_id(preq->req);
    enum leme_control_request_op op = leme_control_request_operation(preq->req);
    struct leme_control_frame *reply_frame = NULL;
    bool watch_published = false;
    struct leme_public_model *model = leme_control_context_model(peer->context);
    struct leme_public_text model_inst = leme_public_model_instance(model);

    if (!peer->negotiated) {
      if (op != LEME_CONTROL_HELLO) {
        struct leme_control_error err = {
            .code = LEME_CONTROL_INVALID_REQUEST,
            .phase = LEME_CONTROL_VALIDATE,
            .message = "must negotiate with hello first",
        };
        (void)snprintf(err.expr_path, sizeof(err.expr_path), "/op");
        leme_control_reply_create_error(
            peer->account, peer->limits.response_bytes, req_id.data,
            req_id.length, model_inst.data, model_inst.length, NULL, 0, &err,
            &reply_frame);
      } else {
        enum leme_control_code disp_code =
            dispatch_peer_request(peer, preq->req, &reply_frame);
        if (disp_code == LEME_CONTROL_OK && reply_frame != NULL) {
          peer->negotiated = true;
          peer->state = LEME_PEER_STATE_READY;
        }
      }
    } else {
      if (op == LEME_CONTROL_HELLO) {
        struct leme_control_error err = {
            .code = LEME_CONTROL_INVALID_REQUEST,
            .phase = LEME_CONTROL_VALIDATE,
            .message = "already negotiated",
        };
        (void)snprintf(err.expr_path, sizeof(err.expr_path), "/op");
        leme_control_reply_create_error(
            peer->account, peer->limits.response_bytes, req_id.data,
            req_id.length, model_inst.data, model_inst.length, NULL, 0, &err,
            &reply_frame);
      } else {
        struct leme_public_text req_inst =
            leme_control_request_instance(preq->req);
        if (req_inst.length != model_inst.length ||
            memcmp(req_inst.data, model_inst.data, req_inst.length) != 0) {
          struct leme_control_error err = {
              .code = LEME_CONTROL_STALE_INSTANCE,
              .phase = LEME_CONTROL_VALIDATE,
              .message = "stale instance",
          };
          (void)snprintf(err.expr_path, sizeof(err.expr_path), "/instance");
          leme_control_reply_create_error(
              peer->account, peer->limits.response_bytes, req_id.data,
              req_id.length, model_inst.data, model_inst.length, NULL, 0, &err,
              &reply_frame);
        } else if (op == LEME_CONTROL_WATCH || op == LEME_CONTROL_UNWATCH) {
          watch_published = leme_control_watch_peer_request(
                                peer, preq->req, preq->slot) == LEME_CONTROL_OK;
        } else {
          dispatch_peer_request(peer, preq->req, &reply_frame);
        }
      }
    }

    if (watch_published || peer->state == LEME_PEER_STATE_CLOSED) {
      leme_control_frame_destroy(reply_frame);
      pending_req_destroy(peer, preq);
      leme_control_peer_flush(peer);
      return;
    }

    if (reply_frame == NULL && op == LEME_CONTROL_ACT) {
      pending_req_destroy(peer, preq);
      peer_close_socket(peer);
      return;
    }
    if (reply_frame == NULL) {
      struct leme_control_error fb_err = {
          .code = LEME_CONTROL_RESOURCE_LIMIT,
          .phase = LEME_CONTROL_EXECUTE,
          .message = "response limit exceeded",
      };
      (void)snprintf(fb_err.expr_path, sizeof(fb_err.expr_path), "/expr");
      leme_control_reply_create_error(
          peer->account, peer->limits.response_bytes, req_id.data,
          req_id.length, model_inst.data, model_inst.length, NULL, 0, &fb_err,
          &reply_frame);
    }

    if (reply_frame != NULL) {
      if (preq->slot >= 0 && preq->slot < 16) {
        peer->outstanding[preq->slot].frame = reply_frame;
      }
      enum leme_control_code qcode =
          peer->queue->reserved_node != NULL
              ? leme_control_queue_push_reserved(peer->queue, reply_frame)
              : leme_control_queue_push(peer->queue, reply_frame);
      leme_control_queue_cancel_reservation(peer->queue,
                                            peer->queue->reserved_bytes);
      if (qcode != LEME_CONTROL_OK) {
        if (preq->slot >= 0 && preq->slot < 16) {
          peer->outstanding[preq->slot].active = false;
          peer->outstanding[preq->slot].frame = NULL;
          peer->outstanding[preq->slot].id_len = 0;
          peer->outstanding[preq->slot].id[0] = '\0';
          if (peer->outstanding_count > 0) {
            peer->outstanding_count--;
          }
        }
        leme_control_frame_destroy(reply_frame);
        pending_req_destroy(peer, preq);
        peer_close_socket(peer);
        return;
      }
    } else {
      if (preq->slot >= 0 && preq->slot < 16) {
        peer->outstanding[preq->slot].active = false;
        peer->outstanding[preq->slot].frame = NULL;
        peer->outstanding[preq->slot].id_len = 0;
        peer->outstanding[preq->slot].id[0] = '\0';
        if (peer->outstanding_count > 0) {
          peer->outstanding_count--;
        }
      }
      pending_req_destroy(peer, preq);
      peer_close_socket(peer);
      return;
    }

    pending_req_destroy(peer, preq);
  }

  leme_control_peer_flush(peer);
}

bool leme_control_peer_has_work(const struct leme_control_peer *peer) {
  if (peer == NULL || peer->state == LEME_PEER_STATE_CLOSED) {
    return false;
  }
  return !wl_list_empty(&peer->pending_requests) ||
         (peer->state == LEME_PEER_STATE_READY &&
          leme_control_watch_has_work(peer->watches));
}

static int leme_control_peer_handle_fd(int fd, uint32_t len, void *data) {
  struct leme_control_peer *peer = data;
  (void)fd;
  uint32_t mask = len;

  if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
    peer_close_socket(peer);
    return 0;
  }

  if ((mask & WL_EVENT_WRITABLE) != 0) {
    leme_control_peer_flush(peer);
  }

  if ((mask & WL_EVENT_READABLE) != 0 &&
      peer->state != LEME_PEER_STATE_DRAINING &&
      peer->state != LEME_PEER_STATE_CLOSED) {
    char buf[4096];
    bool should_stop = false;
    struct leme_public_model *model = leme_control_context_model(peer->context);
    struct leme_public_text model_inst = leme_public_model_instance(model);
    uint64_t loop_start_ns = monotonic_now_ns(NULL);
    uint64_t loop_deadline_ns = (peer->limits.deadline_ns > 0)
                                    ? loop_start_ns + peer->limits.deadline_ns
                                    : 0;

    while (!should_stop) {
      if (loop_deadline_ns > 0 && monotonic_now_ns(NULL) >= loop_deadline_ns) {
        break;
      }
      if (peer->outstanding_count >= peer->limits.outstanding) {
        break;
      }
      ssize_t ret = read(peer->fd, buf, sizeof(buf));
      if (ret > 0) {
        size_t n = (size_t)ret;
        size_t offset = 0;
        while (offset < n) {
          size_t consumed = 0;
          struct leme_control_frame *frame = NULL;
          enum leme_control_code fcode = leme_control_framer_feed(
              peer->framer, buf + offset, n - offset, &consumed, &frame);
          offset += consumed;
          if (fcode != LEME_CONTROL_OK) {
            struct leme_control_error err = {
                .code = LEME_CONTROL_RESOURCE_LIMIT,
                .phase = LEME_CONTROL_DECODE,
                .message = "frame limit exceeded",
            };
            struct leme_control_frame *err_frame = NULL;
            leme_control_reply_create_error(
                peer->account, peer->limits.response_bytes, NULL, 0,
                model_inst.data, model_inst.length, NULL, 0, &err, &err_frame);
            if (err_frame != NULL) {
              struct leme_control_pending_req *preq = pending_req_create(peer);
              if (preq != NULL) {
                preq->req = NULL;
                preq->preformed_frame = err_frame;
                preq->slot = -1;
                preq->fatal = true;
                wl_list_insert(peer->pending_requests.prev, &preq->link);
              } else {
                leme_control_frame_destroy(err_frame);
              }
            }
            peer->state = LEME_PEER_STATE_DRAINING;
            should_stop = true;
            break;
          }
          if (frame == NULL) {
            continue;
          }

          const char *fbytes = leme_control_frame_bytes(frame);
          size_t flen = leme_control_frame_length(frame);
          if (flen == 0) {
            leme_control_frame_destroy(frame);
            continue;
          }

          struct leme_control_document *doc = NULL;
          struct leme_control_error err = {0};
          struct leme_control_meter dmeter = {
              .remaining = peer->limits.work_units > 0 ? peer->limits.work_units
                                                       : 100000,
              .deadline_ns = loop_deadline_ns,
              .context = NULL,
              .now_ns = monotonic_now_ns,
          };
          enum leme_control_code dcode = leme_control_decode(
              peer->account,
              (struct leme_public_text){.data = fbytes, .length = flen},
              &peer->limits, &dmeter, &doc, &err);
          if (dcode != LEME_CONTROL_OK) {
            struct leme_control_frame *err_frame = NULL;
            leme_control_reply_create_error(
                peer->account, peer->limits.response_bytes, NULL, 0,
                model_inst.data, model_inst.length, NULL, 0, &err, &err_frame);
            if (err_frame != NULL) {
              struct leme_control_pending_req *preq = pending_req_create(peer);
              if (preq != NULL) {
                preq->req = NULL;
                preq->preformed_frame = err_frame;
                preq->slot = -1;
                preq->fatal = true;
                wl_list_insert(peer->pending_requests.prev, &preq->link);
              } else {
                leme_control_frame_destroy(err_frame);
              }
            }
            peer->state = LEME_PEER_STATE_DRAINING;
            leme_control_frame_destroy(frame);
            should_stop = true;
            break;
          }

          struct leme_control_request *req = NULL;
          enum leme_control_code rcode =
              leme_control_request_create(&doc, &req, &err);
          if (rcode != LEME_CONTROL_OK) {
            const char *id_ptr = NULL;
            size_t id_len = 0;
            struct leme_public_text id_text = {0};
            if (doc != NULL) {
              const struct leme_public_value *root =
                  leme_control_document_value(doc);
              const struct leme_public_value *id_val =
                  root != NULL ? leme_public_get(root, LEME_PUBLIC_TEXT("id"))
                               : NULL;
              if (id_val != NULL &&
                  leme_public_as_text(id_val, &id_text) == LEME_PUBLIC_OK) {
                id_ptr = id_text.data;
                id_len = id_text.length;
              }
            }
            struct leme_control_frame *err_frame = NULL;
            leme_control_reply_create_error(
                peer->account, peer->limits.response_bytes, id_ptr, id_len,
                model_inst.data, model_inst.length, NULL, 0, &err, &err_frame);
            if (err_frame != NULL) {
              struct leme_control_pending_req *preq = pending_req_create(peer);
              if (preq != NULL) {
                preq->req = NULL;
                preq->preformed_frame = err_frame;
                preq->slot = -1;
                preq->fatal = (rcode == LEME_CONTROL_UNSUPPORTED_VERSION);
                wl_list_insert(peer->pending_requests.prev, &preq->link);
              } else {
                leme_control_frame_destroy(err_frame);
              }
            }
            if (rcode == LEME_CONTROL_UNSUPPORTED_VERSION) {
              peer->state = LEME_PEER_STATE_DRAINING;
            }
            if (doc != NULL) {
              leme_control_document_destroy(doc);
            }
            leme_control_frame_destroy(frame);
            if (peer->state == LEME_PEER_STATE_DRAINING) {
              should_stop = true;
              break;
            }
            continue;
          }

          if (doc != NULL) {
            leme_control_document_destroy(doc);
          }

          struct leme_public_text req_id = leme_control_request_id(req);
          if (peer_has_outstanding_id(peer, req_id.data, req_id.length)) {
            struct leme_control_error dup_err = {
                .code = LEME_CONTROL_INVALID_REQUEST,
                .phase = LEME_CONTROL_DECODE,
                .message = "duplicate request id",
            };
            struct leme_control_frame *dup_frame = NULL;
            leme_control_reply_create_error(
                peer->account, peer->limits.response_bytes, NULL, 0,
                model_inst.data, model_inst.length, NULL, 0, &dup_err,
                &dup_frame);
            if (dup_frame != NULL) {
              struct leme_control_pending_req *preq = pending_req_create(peer);
              if (preq != NULL) {
                preq->req = NULL;
                preq->preformed_frame = dup_frame;
                preq->slot = -1;
                preq->fatal = true;
                wl_list_insert(peer->pending_requests.prev, &preq->link);
              } else {
                leme_control_frame_destroy(dup_frame);
              }
            }
            peer->state = LEME_PEER_STATE_DRAINING;
            leme_control_request_destroy(req);
            leme_control_frame_destroy(frame);
            should_stop = true;
            break;
          }

          int slot = peer_add_outstanding(peer, req_id.data, req_id.length);
          if (slot < 0) {
            struct leme_control_error lim_err = {
                .code = LEME_CONTROL_RESOURCE_LIMIT,
                .phase = LEME_CONTROL_DECODE,
                .message = "too many outstanding requests",
            };
            struct leme_control_frame *lim_frame = NULL;
            leme_control_reply_create_error(
                peer->account, peer->limits.response_bytes, req_id.data,
                req_id.length, model_inst.data, model_inst.length, NULL, 0,
                &lim_err, &lim_frame);
            if (lim_frame != NULL) {
              struct leme_control_pending_req *preq = pending_req_create(peer);
              if (preq != NULL) {
                preq->req = NULL;
                preq->preformed_frame = lim_frame;
                preq->slot = -1;
                preq->fatal = true;
                wl_list_insert(peer->pending_requests.prev, &preq->link);
              } else {
                leme_control_frame_destroy(lim_frame);
              }
            }
            leme_control_request_destroy(req);
            leme_control_frame_destroy(frame);
            peer->state = LEME_PEER_STATE_DRAINING;
            should_stop = true;
            break;
          }

          struct leme_control_pending_req *preq = pending_req_create(peer);
          if (preq == NULL) {
            peer->outstanding[slot].active = false;
            peer->outstanding_count--;
            leme_control_request_destroy(req);
            leme_control_frame_destroy(frame);
            continue;
          }
          preq->req = req;
          preq->preformed_frame = NULL;
          preq->slot = slot;
          preq->fatal = false;
          wl_list_insert(peer->pending_requests.prev, &preq->link);
          leme_control_frame_destroy(frame);
        }
      } else if (ret == 0) {
        peer_close_socket(peer);
        break;
      } else {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        peer_close_socket(peer);
        break;
      }
    }

    if (peer->state != LEME_PEER_STATE_CLOSED &&
        !wl_list_empty(&peer->pending_requests)) {
      const uint64_t previous = leme_control_context_deadline(peer->context);
      const uint64_t deadline = previous != 0 && previous < loop_deadline_ns
                                    ? previous
                                    : loop_deadline_ns;
      leme_control_context_set_deadline(peer->context, deadline);
      leme_control_peer_ready(peer);
      leme_control_context_set_deadline(peer->context, previous);
    }
  }

  return 0;
}

enum leme_control_code
leme_control_peer_open(struct leme_control_context *context,
                       struct wl_event_loop *loop, int fd,
                       struct leme_control_peer **out) {
  if (context == NULL || loop == NULL || fd < 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out = NULL;

  const struct leme_control_limits *ctx_limits =
      leme_control_context_limits(context);
  struct leme_control_limits limits =
      ctx_limits != NULL ? *ctx_limits : leme_control_limits_default();

  struct leme_public_budget *ctx_account =
      leme_control_context_account(context);
  struct leme_public_budget *peer_account = NULL;
  enum leme_public_status bst = LEME_PUBLIC_INVALID;
  if (ctx_account != NULL) {
    bst = leme_public_budget_child(ctx_account, limits.retained_bytes,
                                   &peer_account);
  } else {
    bst = leme_public_budget_create(limits.retained_bytes, NULL, &peer_account);
  }
  if (bst != LEME_PUBLIC_OK) {
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  struct leme_control_peer *peer = calloc(1, sizeof(*peer));
  if (peer == NULL) {
    leme_public_budget_unref(peer_account);
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  peer->context = context;
  peer->loop = loop;
  peer->fd = fd;
  peer->account = peer_account;
  peer->limits = limits;
  peer->state = LEME_PEER_STATE_NEGOTIATING;
  peer->negotiated = false;
  wl_list_init(&peer->link);
  wl_list_init(&peer->runnable_link);
  wl_list_init(&peer->pending_requests);

  enum leme_control_code code = leme_control_framer_create(
      peer->account, peer->limits.request_bytes, &peer->framer);
  if (code != LEME_CONTROL_OK) {
    leme_control_peer_destroy(peer);
    return code;
  }

  code = leme_control_queue_create(peer->account, peer->limits.output_bytes,
                                   &peer->queue);
  if (code != LEME_CONTROL_OK) {
    leme_control_peer_destroy(peer);
    return code;
  }

  leme_control_queue_set_completion(peer->queue, on_peer_frame_complete, peer);

  struct leme_control_scheduler *scheduler =
      leme_control_context_scheduler(context);
  if (scheduler == NULL) {
    scheduler = leme_control_scheduler_create(context, loop);
    if (scheduler == NULL) {
      leme_control_peer_destroy(peer);
      return LEME_CONTROL_OUT_OF_MEMORY;
    }
    leme_control_context_set_scheduler(context, scheduler,
                                       leme_control_scheduler_destroy);
  }
  if (!leme_control_scheduler_add_peer(scheduler, peer)) {
    leme_control_peer_destroy(peer);
    return LEME_CONTROL_OUT_OF_MEMORY;
  }
  peer->scheduler = scheduler;

  peer->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
                                      leme_control_peer_handle_fd, peer);
  if (peer->source == NULL) {
    leme_control_peer_destroy(peer);
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  *out = peer;
  return LEME_CONTROL_OK;
}

void leme_control_peer_destroy(struct leme_control_peer *peer) {
  if (peer == NULL) {
    return;
  }
  if (peer->scheduler != NULL) {
    leme_control_scheduler_remove_peer(peer->scheduler, peer);
    peer->scheduler = NULL;
  }
  peer_close_socket(peer);
  leme_control_watch_set_destroy(peer->watches);
  peer->watches = NULL;
  struct leme_control_pending_req *preq = NULL;
  struct leme_control_pending_req *tmp = NULL;
  wl_list_for_each_safe(preq, tmp, &peer->pending_requests, link) {
    wl_list_remove(&preq->link);
    pending_req_destroy(peer, preq);
  }
  wl_list_init(&peer->pending_requests);
  if (peer->queue != NULL) {
    leme_control_queue_destroy(peer->queue);
    peer->queue = NULL;
  }
  if (peer->framer != NULL) {
    leme_control_framer_destroy(peer->framer);
    peer->framer = NULL;
  }
  if (peer->account != NULL) {
    leme_public_budget_unref(peer->account);
    peer->account = NULL;
  }
  free(peer);
}

void leme_control_peer_ready(struct leme_control_peer *peer) {
  if (peer != NULL && peer->scheduler != NULL) {
    leme_control_scheduler_peer_ready(peer->scheduler, peer);
  }
}

enum leme_control_code
leme_control_peer_reserve_reply(struct leme_control_peer *peer, size_t bytes) {
  if (peer == NULL || peer->queue == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  return leme_control_queue_reserve(peer->queue, bytes);
}

int leme_control_peer_fd(const struct leme_control_peer *peer) {
  return peer != NULL ? peer->fd : -1;
}
