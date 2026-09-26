#include "ipc/ipc.h"

#include "control/control.h"
#include "control/limits.h"
#include "core/server.h"
#include "ipc/connection.h"
#include "ipc/connection-internal.h"
#include "ipc/schedule.h"
#include "public/budget.h"
#include "public/model.h"
#include "public/server.h"
#include "shell/control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

struct leme_ipc_client {
  struct leme_ipc *ipc;
  struct leme_control_peer *peer;
  struct wl_list link;
};

struct leme_ipc {
  struct leme_server *server;
  int fd;
  char *path;
  dev_t dev;
  ino_t ino;
  char *saved_env_socket;
  struct wl_event_source *source;
  struct leme_public_source public_source;
  struct leme_control_domain control_domain;
  struct leme_control_context *context;
  struct leme_control_limits limits;
  struct wl_list clients;
  size_t client_count;
};

static void leme_ipc_prune_closed(struct leme_ipc *ipc) {
  if (ipc == NULL) {
    return;
  }
  struct leme_ipc_client *client = NULL;
  struct leme_ipc_client *tmp = NULL;
  wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
    if (client->peer == NULL || client->peer->state == LEME_PEER_STATE_CLOSED) {
      wl_list_remove(&client->link);
      if (client->peer != NULL) {
        leme_control_peer_destroy(client->peer);
        client->peer = NULL;
      }
      free(client);
      if (ipc->client_count > 0) {
        ipc->client_count--;
      }
    }
  }
}

static int leme_ipc_handle_listen(int fd, uint32_t len, void *data) {
  struct leme_ipc *ipc = data;
  (void)fd;

  if ((len & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
    return 0;
  }

  leme_ipc_prune_closed(ipc);

  int client_fd = accept(ipc->fd, NULL, NULL);
  if (client_fd < 0) {
    return 0;
  }

  if (fcntl(client_fd, F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(client_fd, F_SETFL, O_NONBLOCK) != 0) {
    close(client_fd);
    return 0;
  }

  if (ipc->client_count >= ipc->limits.clients) {
    close(client_fd);
    return 0;
  }

  struct wl_event_loop *loop =
      wl_display_get_event_loop(ipc->server->display);
  struct leme_control_peer *peer = NULL;
  enum leme_control_code code =
      leme_control_peer_open(ipc->context, loop, client_fd, &peer);
  if (code != LEME_CONTROL_OK || peer == NULL) {
    close(client_fd);
    return 0;
  }

  struct leme_ipc_client *client = calloc(1, sizeof(*client));
  if (client == NULL) {
    leme_control_peer_destroy(peer);
    return 0;
  }

  client->ipc = ipc;
  client->peer = peer;
  wl_list_insert(&ipc->clients, &client->link);
  ipc->client_count++;
  return 0;
}

static bool leme_ipc_build_path(const struct leme_server *server, char *path,
                                size_t size) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime == NULL || runtime[0] == '\0' || server->socket == NULL ||
      server->socket[0] == '\0') {
    return false;
  }
  int written = snprintf(path, size, "%s/leme-%s.sock", runtime, server->socket);
  return written >= 0 && (size_t)written < size;
}

static bool leme_ipc_probe_and_prepare(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    return false;
  }

  if (!S_ISSOCK(st.st_mode)) {
    wlr_log(WLR_ERROR, "leme: %s is not a socket", path);
    return false;
  }

  if (st.st_uid != getuid()) {
    wlr_log(WLR_ERROR, "leme: %s has foreign owner", path);
    return false;
  }

  struct sockaddr_un address = {0};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path));

  int probe_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (probe_fd < 0) {
    return false;
  }

  int conn = connect(probe_fd, (const struct sockaddr *)&address, sizeof(address));
  int conn_err = errno;
  close(probe_fd);

  if (conn == 0) {
    wlr_log(WLR_ERROR, "leme: %s is already served", path);
    return false;
  }

  if (conn_err == ECONNREFUSED) {
    if (unlink(path) == 0) {
      return true;
    }
  }

  wlr_log(WLR_ERROR, "leme: probe failure for %s", path);
  return false;
}

bool leme_ipc_init(struct leme_server *server) {
  char path[PATH_MAX];
  if (!leme_ipc_build_path(server, path, sizeof(path))) {
    wlr_log(WLR_INFO, "%s",
            "leme: no runtime directory, control interface disabled");
    return true;
  }

  struct sockaddr_un address = {0};
  if (strlen(path) >= sizeof(address.sun_path)) {
    wlr_log(WLR_ERROR, "%s", "leme: control interface path is too long");
    return true;
  }

  if (!leme_ipc_probe_and_prepare(path)) {
    return true;
  }

  struct leme_ipc *ipc = calloc(1, sizeof(*ipc));
  if (ipc == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to allocate control interface");
    return true;
  }

  ipc->server = server;
  ipc->fd = -1;
  wl_list_init(&ipc->clients);
  ipc->limits = leme_control_limits_default();

  const char *existing_env = getenv("LEME_SOCKET");
  if (existing_env != NULL) {
    ipc->saved_env_socket = strdup(existing_env);
  }

  ipc->path = strdup(path);
  if (ipc->path == NULL) {
    goto fail;
  }

  ipc->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (ipc->fd < 0) {
    goto fail;
  }

  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path));

  mode_t saved = umask(0077);
  bool bound =
      bind(ipc->fd, (const struct sockaddr *)&address, sizeof(address)) == 0;
  umask(saved);
  if (!bound) {
    goto fail;
  }

  if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
    goto fail;
  }

  struct stat bound_st;
  if (lstat(path, &bound_st) != 0) {
    goto fail;
  }
  ipc->dev = bound_st.st_dev;
  ipc->ino = bound_st.st_ino;

  if (listen(ipc->fd, (int)ipc->limits.clients) != 0) {
    goto fail;
  }

  ipc->public_source = leme_public_server_source(server);
  ipc->control_domain = leme_server_control_domain(server);

  struct leme_public_budget *account = NULL;
  struct leme_public_budget *model_budget =
      server->public_model != NULL
          ? leme_public_model_budget_ref(server->public_model)
          : NULL;
  if (model_budget != NULL) {
    if (leme_public_budget_child(model_budget, ipc->limits.total_bytes,
                                 &account) != LEME_PUBLIC_OK) {
      leme_public_budget_unref(model_budget);
      goto fail;
    }
    leme_public_budget_unref(model_budget);
  } else {
    if (leme_public_budget_create(ipc->limits.total_bytes, NULL, &account) !=
        LEME_PUBLIC_OK) {
      goto fail;
    }
  }

  enum leme_control_code code = leme_control_context_create(
      server, server->public_model, &ipc->public_source, account,
      &ipc->limits, &ipc->control_domain, &ipc->context);
  leme_public_budget_unref(account);
  if (code != LEME_CONTROL_OK) {
    goto fail;
  }

  struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
  struct leme_control_scheduler *scheduler =
      leme_control_scheduler_create(ipc->context, loop);
  if (scheduler == NULL) {
    goto fail;
  }
  leme_control_context_set_scheduler(ipc->context, scheduler,
                                     leme_control_scheduler_destroy);

  ipc->source = wl_event_loop_add_fd(loop, ipc->fd, WL_EVENT_READABLE,
                                     leme_ipc_handle_listen, ipc);
  if (ipc->source == NULL) {
    goto fail;
  }

  if (setenv("LEME_SOCKET", path, 1) != 0) {
    goto fail;
  }

  server->ipc = ipc;
  wlr_log(WLR_INFO, "leme: LEME_SOCKET=%s", path);
  return true;

fail:
  wlr_log(WLR_ERROR, "%s", "leme: failed to create the control interface");
  if (ipc->source != NULL) {
    wl_event_source_remove(ipc->source);
  }
  if (ipc->fd >= 0) {
    close(ipc->fd);
  }
  if (ipc->path != NULL) {
    struct stat check_st;
    if (ipc->ino != 0 && lstat(ipc->path, &check_st) == 0 &&
        check_st.st_dev == ipc->dev && check_st.st_ino == ipc->ino) {
      unlink(ipc->path);
    }
    free(ipc->path);
  }
  if (ipc->context != NULL) {
    leme_control_context_destroy(ipc->context);
  }
  if (ipc->saved_env_socket != NULL) {
    free(ipc->saved_env_socket);
  }
  free(ipc);
  return true;
}

void leme_ipc_finish(struct leme_server *server) {
  if (server == NULL || server->ipc == NULL) {
    return;
  }
  struct leme_ipc *ipc = server->ipc;

  struct leme_ipc_client *client = NULL;
  struct leme_ipc_client *tmp = NULL;
  wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
    wl_list_remove(&client->link);
    if (client->peer != NULL) {
      leme_control_peer_destroy(client->peer);
    }
    free(client);
  }

  if (ipc->source != NULL) {
    wl_event_source_remove(ipc->source);
    ipc->source = NULL;
  }

  if (ipc->fd >= 0) {
    close(ipc->fd);
    ipc->fd = -1;
  }

  if (ipc->path != NULL) {
    struct stat check_st;
    if (ipc->ino != 0 && lstat(ipc->path, &check_st) == 0 &&
        check_st.st_dev == ipc->dev && check_st.st_ino == ipc->ino) {
      unlink(ipc->path);
    }
    free(ipc->path);
    ipc->path = NULL;
  }

  if (ipc->saved_env_socket != NULL) {
    setenv("LEME_SOCKET", ipc->saved_env_socket, 1);
    free(ipc->saved_env_socket);
    ipc->saved_env_socket = NULL;
  } else {
    unsetenv("LEME_SOCKET");
  }

  if (ipc->context != NULL) {
    leme_control_context_destroy(ipc->context);
    ipc->context = NULL;
  }

  free(ipc);
  server->ipc = NULL;
}

void leme_ipc_invalidate(struct leme_server *server) {
  if (server == NULL || server->ipc == NULL) {
    return;
  }
  struct leme_ipc *ipc = server->ipc;
  if (ipc->context != NULL) {
    struct leme_control_scheduler *sched =
        leme_control_context_scheduler(ipc->context);
    if (sched != NULL) {
      leme_control_scheduler_defer(sched);
    }
  }
}

const char *leme_ipc_socket_path(const struct leme_server *server) {
  return (server == NULL || server->ipc == NULL) ? NULL : server->ipc->path;
}
