#include "timao/transport.h"
#include "control/memory.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct timao_transport {
  struct timao_endpoint endpoint;
  struct timao_client *client;
  uint64_t epoch;
  int fd;
  bool connecting;
};

static enum timao_client_cause cause(int error) {
  if (error == EACCES || error == EPERM || error == ELOOP)
    return TIMAO_CLIENT_PERMISSION;
  if (error == ENOMEM || error == ENOBUFS || error == EMFILE || error == ENFILE)
    return TIMAO_CLIENT_RESOURCE;
  return TIMAO_CLIENT_IO_ERROR;
}

static void lost(struct timao_transport *transport,
                 enum timao_client_cause why) {
  timao_client_lost(
      transport->client,
      (struct timao_client_loss){.epoch = transport->epoch, .cause = why});
}

static void close_connection(struct timao_transport *transport) {
  const uint64_t epoch = transport->fd >= 0
                             ? transport->epoch
                             : timao_client_connect_epoch(transport->client);
  if (transport->fd >= 0) {
    const int status = close(transport->fd);
    const int error = errno;
    transport->fd = -1;
    if (status < 0 && error != EINTR)
      lost(transport, cause(error));
  }
  transport->connecting = false;
  timao_client_closed(transport->client, epoch);
}

static void opened(struct timao_transport *transport) {
  struct ucred credentials = {0};
  socklen_t length = sizeof(credentials);
  if (getsockopt(transport->fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                 &length) < 0) {
    lost(transport, cause(errno));
    return;
  }
  if (length != sizeof(credentials) || credentials.uid != geteuid()) {
    lost(transport, TIMAO_CLIENT_PERMISSION);
    return;
  }
  transport->connecting = false;
  if (timao_client_opened(transport->client, transport->epoch) !=
          LEME_PUBLIC_OK &&
      !timao_client_wants_close(transport->client))
    lost(transport, TIMAO_CLIENT_IO_ERROR);
}

static void connect_socket(struct timao_transport *transport) {
  transport->epoch = timao_client_connect_epoch(transport->client);
  struct stat info = {0};
  if (lstat(transport->endpoint.address.sun_path, &info) < 0) {
    lost(transport, cause(errno));
    return;
  }
  if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0777) != 0600) {
    lost(transport, TIMAO_CLIENT_PERMISSION);
    return;
  }
  transport->fd =
      socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (transport->fd < 0) {
    lost(transport, cause(errno));
    return;
  }
  if (connect(transport->fd,
              (const struct sockaddr *)&transport->endpoint.address,
              transport->endpoint.length) == 0) {
    opened(transport);
    return;
  }
  const int error = errno;
  if (error == EINPROGRESS)
    transport->connecting = true;
  else
    lost(transport, cause(error));
}

static void finish_connect(struct timao_transport *transport) {
  int error = 0;
  socklen_t length = sizeof(error);
  if (getsockopt(transport->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
    lost(transport, cause(errno));
    return;
  }
  if (length != sizeof(error) || error != 0) {
    lost(transport, cause(error));
    return;
  }
  opened(transport);
}

static void read_socket(struct timao_transport *transport) {
  for (size_t turn = 0; turn < 4; ++turn) {
    char bytes[4096] = {0};
    const ssize_t count =
        recv(transport->fd, bytes, sizeof(bytes), MSG_DONTWAIT);
    if (count < 0) {
      const int error = errno;
      if (error == EINTR)
        continue;
      if (error != EAGAIN && error != EWOULDBLOCK)
        lost(transport, cause(error));
      return;
    }
    if (count == 0) {
      lost(transport, TIMAO_CLIENT_EOF);
      return;
    }
    size_t used = 0;
    const enum leme_public_status status = timao_client_feed(
        transport->client, transport->epoch,
        (struct leme_public_text){bytes, (size_t)count}, &used);
    if (status != LEME_PUBLIC_OK || used != (size_t)count) {
      if (!timao_client_wants_close(transport->client))
        lost(transport, TIMAO_CLIENT_PROTOCOL);
      return;
    }
    if (timao_client_wants_close(transport->client))
      return;
  }
}

static void write_socket(struct timao_transport *transport) {
  struct timao_client_ticket ticket = {0};
  struct leme_public_text bytes = {0};
  if (timao_client_peek_write(transport->client, &ticket, &bytes) !=
          LEME_PUBLIC_OK ||
      bytes.length == 0)
    return;
  const size_t length = bytes.length > 16384 ? 16384 : bytes.length;
  const ssize_t count =
      send(transport->fd, bytes.data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
  if (count < 0) {
    const int error = errno;
    if (error != EINTR && error != EAGAIN && error != EWOULDBLOCK)
      lost(transport, cause(error));
    return;
  }
  if (timao_client_written(transport->client, ticket, (size_t)count) !=
      LEME_PUBLIC_OK)
    lost(transport, TIMAO_CLIENT_AMBIGUOUS_IO);
}

enum leme_public_status timao_transport_create(
    struct leme_public_budget *account, const struct timao_endpoint *endpoint,
    struct timao_client *client, struct timao_transport **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (account == NULL || endpoint == NULL || client == NULL ||
      endpoint->address.sun_family != AF_UNIX)
    return LEME_PUBLIC_INVALID;
  const size_t length =
      strnlen(endpoint->address.sun_path, sizeof(endpoint->address.sun_path));
  if (length == 0 || length == sizeof(endpoint->address.sun_path) ||
      (size_t)endpoint->length !=
          offsetof(struct sockaddr_un, sun_path) + length + 1)
    return LEME_PUBLIC_INVALID;
  struct timao_transport *transport =
      leme_control_alloc(account, sizeof(*transport));
  if (transport == NULL)
    return errno == ENOSPC || errno == EOVERFLOW ? LEME_PUBLIC_LIMIT
                                                 : LEME_PUBLIC_OOM;
  *transport = (struct timao_transport){
      .endpoint = *endpoint, .client = client, .fd = -1};
  *out = transport;
  return LEME_PUBLIC_OK;
}

void timao_transport_destroy(struct timao_transport *transport) {
  if (transport == NULL)
    return;
  transport->epoch = timao_client_connect_epoch(transport->client);
  lost(transport, TIMAO_CLIENT_CANCELLED);
  close_connection(transport);
  leme_control_free(transport);
}

int timao_transport_fd(const struct timao_transport *transport) {
  return transport != NULL ? transport->fd : -1;
}

short timao_transport_events(struct timao_transport *transport) {
  if (transport == NULL || transport->fd < 0)
    return 0;
  if (transport->connecting || timao_client_wants_close(transport->client))
    return POLLIN | POLLOUT;
  struct timao_client_ticket ticket = {0};
  struct leme_public_text bytes = {0};
  if (timao_client_peek_write(transport->client, &ticket, &bytes) !=
      LEME_PUBLIC_OK)
    return POLLIN;
  return bytes.length != 0 || timao_client_wants_close(transport->client)
             ? POLLIN | POLLOUT
             : POLLIN;
}

void timao_transport_step(struct timao_transport *transport, short revents) {
  if (transport == NULL)
    return;
  timao_client_tick(transport->client);
  if (timao_client_wants_close(transport->client)) {
    close_connection(transport);
    revents = 0;
  }
  if (timao_client_wants_connect(transport->client) && transport->fd < 0) {
    connect_socket(transport);
    revents = 0;
  }
  if (transport->fd >= 0 && !timao_client_wants_close(transport->client)) {
    if (revents & POLLNVAL) {
      lost(transport, TIMAO_CLIENT_IO_ERROR);
    } else if (transport->connecting) {
      if (revents & (POLLIN | POLLOUT | POLLERR | POLLHUP))
        finish_connect(transport);
    } else {
      if (revents & (POLLIN | POLLERR | POLLHUP))
        read_socket(transport);
      if (!timao_client_wants_close(transport->client) && (revents & POLLOUT) &&
          !(revents & POLLHUP))
        write_socket(transport);
    }
  }
  if (timao_client_wants_close(transport->client))
    close_connection(transport);
}
