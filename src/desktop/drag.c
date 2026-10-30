#include "desktop/drag.h"

#include "core/server.h"
#include "desktop/desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct leme_desk_drag {
  struct wlr_data_source source;
  struct wlr_seat_client client;
  struct leme_server *server;
  struct wlr_drag *drag;
  struct wl_listener drag_destroy;
  struct wlr_scene_tree *image;
  char **paths;
  size_t count;
  char *uris;
  size_t length;
  bool cancelled;
};

struct leme_desk_drag_writer {
  struct wl_event_source *source;
  char *data;
  size_t length;
  size_t offset;
  int fd;
};

static bool leme_desk_drag_write(int fd, const char *data, size_t length,
                                 size_t *offset) {
  while (*offset < length) {
    const ssize_t written = write(fd, data + *offset, length - *offset);
    if (written > 0) {
      *offset += (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
  }
  return true;
}

static void leme_desk_drag_writer_done(struct leme_desk_drag_writer *writer) {
  if (writer->offset < writer->length) {
    wlr_log(WLR_ERROR, "%s", "leme: could not send dragged desktop files");
  }
  wl_event_source_remove(writer->source);
  (void)close(writer->fd);
  free(writer->data);
  free(writer);
}

static int leme_desk_drag_writable(int fd, uint32_t mask, void *data) {
  struct leme_desk_drag_writer *writer = data;
  if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0 ||
      !leme_desk_drag_write(fd, writer->data, writer->length,
                            &writer->offset) ||
      writer->offset == writer->length) {
    leme_desk_drag_writer_done(writer);
  }
  return 0;
}

static void leme_desk_drag_send(struct wlr_data_source *source,
                                const char *mime_type, int32_t fd) {
  struct leme_desk_drag *drag = wl_container_of(source, drag, source);
  size_t offset = 0;
  const int flags = fcntl(fd, F_GETFL);
  if (strcmp(mime_type, "text/uri-list") != 0 || flags < 0 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
      !leme_desk_drag_write(fd, drag->uris, drag->length, &offset) ||
      offset == drag->length) {
    if (offset < drag->length) {
      wlr_log(WLR_ERROR, "%s", "leme: could not send dragged desktop files");
    }
    (void)close(fd);
    return;
  }
  struct leme_desk_drag_writer *writer = calloc(1, sizeof(*writer));
  char *rest = writer == NULL ? NULL : malloc(drag->length - offset);
  if (rest == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: could not send dragged desktop files");
    free(writer);
    (void)close(fd);
    return;
  }
  memcpy(rest, drag->uris + offset, drag->length - offset);
  *writer = (struct leme_desk_drag_writer){
      .data = rest, .length = drag->length - offset, .fd = fd};
  writer->source =
      wl_event_loop_add_fd(wl_display_get_event_loop(drag->server->display), fd,
                           WL_EVENT_WRITABLE, leme_desk_drag_writable, writer);
  if (writer->source == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: could not send dragged desktop files");
    free(rest);
    free(writer);
    (void)close(fd);
  }
}

static void leme_desk_drag_dnd_finish(struct wlr_data_source *source) {
  (void)source;
}

static void leme_desk_drag_free(struct wlr_data_source *source) {
  struct leme_desk_drag *drag = wl_container_of(source, drag, source);
  if (drag->drag != NULL) {
    wl_list_remove(&drag->drag_destroy.link);
  }
  if (drag->image != NULL) {
    wlr_scene_node_destroy(&drag->image->node);
  }
  if (drag->server->desk_drag == drag) {
    drag->server->desk_drag = NULL;
  }
  g_strfreev(drag->paths);
  g_free(drag->uris);
  free(drag);
}

static const struct wlr_data_source_impl leme_desk_drag_impl = {
    .send = leme_desk_drag_send,
    .dnd_finish = leme_desk_drag_dnd_finish,
    .destroy = leme_desk_drag_free,
};

static void leme_desk_drag_handle_destroy(struct wl_listener *listener,
                                          void *data) {
  struct leme_desk_drag *drag = wl_container_of(listener, drag, drag_destroy);
  const struct wlr_drag *wlr_drag = data;
  wl_list_remove(&drag->drag_destroy.link);
  drag->drag = NULL;
  if (drag->image != NULL) {
    wlr_scene_node_destroy(&drag->image->node);
    drag->image = NULL;
  }
  leme_desk_drop_feedback(drag->server, NULL, 0);
  if (!wlr_drag->dropped && !drag->cancelled) {
    (void)leme_desk_drop(drag->server, drag->paths, drag->count);
  }
}

static bool leme_desk_drag_uris(struct leme_desk_drag *drag, char *const *paths,
                                size_t count) {
  drag->paths = g_new0(char *, count + 1);
  drag->count = count;
  GString *uris = g_string_new(NULL);
  for (size_t i = 0; i < count; i++) {
    drag->paths[i] = g_strdup(paths[i]);
    char *uri = g_filename_to_uri(paths[i], NULL, NULL);
    if (uri == NULL) {
      g_string_free(uris, true);
      return false;
    }
    g_string_append(uris, uri);
    g_string_append(uris, "\r\n");
    g_free(uri);
  }
  drag->length = uris->len;
  drag->uris = g_string_free(uris, false);
  return true;
}

bool leme_desk_drag_begin(struct leme_server *server, uint32_t time,
                          char *const *paths, size_t count,
                          struct wlr_scene_tree *image) {
  if (!leme_desk_drag_active(server)) {
    leme_desk_drag_finish(server);
  }
  struct leme_desk_drag *drag =
      server == NULL || server->seat == NULL || server->display == NULL ||
              server->cursor == NULL || server->scene_drag == NULL ||
              paths == NULL || count == 0 || server->seat->drag != NULL
          ? NULL
          : calloc(1, sizeof(*drag));
  if (drag == NULL) {
    if (image != NULL) {
      wlr_scene_node_destroy(&image->node);
    }
    return false;
  }
  drag->server = server;
  drag->image = image;
  wlr_data_source_init(&drag->source, &leme_desk_drag_impl);
  drag->source.actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
  char **mime = wl_array_add(&drag->source.mime_types, sizeof(*mime));
  if (mime != NULL) {
    *mime = strdup("text/uri-list");
  }
  drag->client = (struct wlr_seat_client){.seat = server->seat};
  wl_list_init(&drag->client.resources);
  wl_list_init(&drag->client.pointers);
  wl_list_init(&drag->client.keyboards);
  wl_list_init(&drag->client.touches);
  wl_list_init(&drag->client.data_devices);
  wl_signal_init(&drag->client.events.destroy);
  if (mime == NULL || *mime == NULL ||
      !leme_desk_drag_uris(drag, paths, count)) {
    wlr_data_source_destroy(&drag->source);
    return false;
  }
  drag->drag = wlr_drag_create(&drag->client, &drag->source, NULL);
  if (drag->drag == NULL) {
    wlr_data_source_destroy(&drag->source);
    return false;
  }
  drag->drag_destroy.notify = leme_desk_drag_handle_destroy;
  wl_signal_add(&drag->drag->events.destroy, &drag->drag_destroy);
  if (image != NULL) {
    wlr_scene_node_reparent(&image->node, server->scene_drag);
    wlr_scene_node_set_position(&image->node, (int)server->cursor->x,
                                (int)server->cursor->y);
  }
  wlr_seat_pointer_notify_clear_focus(server->seat);
  (void)wlr_seat_pointer_notify_button(server->seat, time, BTN_LEFT,
                                       WL_POINTER_BUTTON_STATE_PRESSED);
  wlr_seat_start_pointer_drag(server->seat, drag->drag,
                              wl_display_next_serial(server->display));
  server->desk_drag = drag;
  return true;
}

bool leme_desk_drag_active(const struct leme_server *server) {
  return server != NULL && server->desk_drag != NULL &&
         server->desk_drag->drag != NULL;
}

void leme_desk_drag_motion(struct leme_server *server) {
  if (!leme_desk_drag_active(server)) {
    return;
  }
  struct leme_desk_drag *drag = server->desk_drag;
  if (drag->image != NULL) {
    wlr_scene_node_set_position(&drag->image->node, (int)server->cursor->x,
                                (int)server->cursor->y);
  }
  leme_desk_drop_feedback(server, drag->paths, drag->count);
}

void leme_desk_drag_cancel(struct leme_server *server) {
  if (leme_desk_drag_active(server)) {
    server->desk_drag->cancelled = true;
    wlr_data_source_destroy(&server->desk_drag->source);
  }
}

void leme_desk_drag_finish(struct leme_server *server) {
  if (server != NULL && server->desk_drag != NULL) {
    server->desk_drag->cancelled = true;
    wlr_data_source_destroy(&server->desk_drag->source);
  }
}
