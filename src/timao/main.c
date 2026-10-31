#include "timao/endpoint.h"
#include "timao/output.h"
#include "timao/request.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char help[] =
    "Usage: timao [--json|--raw] [--socket PATH] FORM\n"
    "  query SOURCE           evaluate an expression once\n"
    "  act SOURCE             run an action expression\n"
    "  watch SOURCE           print the expression again on every change\n"
    "  get ROOT [FIELD...]    read one value, e.g. get session mode\n"
    "  watch ROOT [FIELD...]  stream one value, e.g. watch tags\n"
    "  COMMAND [ARG...]       run a Leme command, e.g. focus_tag 2\n"
    "  --help, --version\n"
    "ROOT is views, tags, outputs, inputs, session, config, runtime or "
    "status.\n";

struct timao_reader {
  int fd;
  char *data;
  size_t used;
  size_t consumed;
  size_t capacity;
};

static int complain(const char *code, const char *message, int status) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  char *text = NULL;
  if (root != NULL && yyjson_mut_obj_add_str(doc, root, "code", code) &&
      yyjson_mut_obj_add_strcpy(doc, root, "message", message)) {
    yyjson_mut_doc_set_root(doc, root);
    text = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);
  }
  if (text != NULL)
    (void)fprintf(stderr, "%s\n", text);
  else
    (void)fprintf(stderr, "timao: %s: %s\n", code, message);
  free(text);
  yyjson_mut_doc_free(doc);
  return status;
}

static int lost(void) {
  return complain("connection", "the compositor closed the connection", 1);
}

static int forward_error(yyjson_val *error) {
  char *text = yyjson_val_write(error, YYJSON_WRITE_NOFLAG, NULL);
  if (text == NULL)
    return complain("protocol", "malformed error reply", 1);
  (void)fprintf(stderr, "%s\n", text);
  free(text);
  const char *phase = yyjson_get_str(yyjson_obj_get(error, "phase"));
  if (phase != NULL &&
      (strcmp(phase, "decode") == 0 || strcmp(phase, "validate") == 0 ||
       strcmp(phase, "preflight") == 0))
    return 2;
  return 1;
}

static int print(enum timao_format format, yyjson_val *value) {
  if (value == NULL || timao_output_value(stdout, format, value))
    return 0;
  if (ferror(stdout))
    return complain("output", "cannot write to stdout", 1);
  return complain("raw_output", "the value has no plain form, use --json", 1);
}

static int connect_socket(const struct timao_endpoint *endpoint, int *fd) {
  struct stat info = {0};
  if (lstat(endpoint->address.sun_path, &info) != 0)
    return complain("connection", strerror(errno), 2);
  if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0777) != 0600)
    return complain("permission", "the socket must be yours and have mode 0600",
                    2);
  *fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (*fd < 0)
    return complain("connection", strerror(errno), 2);
  if (connect(*fd, (const struct sockaddr *)&endpoint->address,
              endpoint->length) != 0)
    return complain("connection", strerror(errno), 2);
  struct ucred credentials = {0};
  socklen_t length = sizeof(credentials);
  if (getsockopt(*fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
      length != sizeof(credentials) || credentials.uid != geteuid())
    return complain("permission", "the compositor runs as another user", 2);
  return 0;
}

static bool send_all(int fd, const char *data, size_t length) {
  while (length > 0) {
    const ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    data += (size_t)sent;
    length -= (size_t)sent;
  }
  return true;
}

static char *read_line(struct timao_reader *r, size_t *length) {
  if (r->consumed > 0) {
    memmove(r->data, r->data + r->consumed, r->used - r->consumed);
    r->used -= r->consumed;
    r->consumed = 0;
  }
  for (;;) {
    char *newline = r->used == 0 ? NULL : memchr(r->data, '\n', r->used);
    if (newline != NULL) {
      *newline = '\0';
      *length = (size_t)(newline - r->data);
      r->consumed = *length + 1;
      return r->data;
    }
    if (r->used == r->capacity) {
      if (r->capacity > SIZE_MAX / 2)
        return NULL;
      const size_t grown = r->capacity == 0 ? 65536 : r->capacity * 2;
      char *larger = realloc(r->data, grown);
      if (larger == NULL)
        return NULL;
      r->data = larger;
      r->capacity = grown;
    }
    const ssize_t got =
        recv(r->fd, r->data + r->used, r->capacity - r->used, 0);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return NULL;
    r->used += (size_t)got;
  }
}

static yyjson_doc *read_message(struct timao_reader *r) {
  size_t length = 0;
  const char *line = read_line(r, &length);
  return line == NULL ? NULL : yyjson_read(line, length, YYJSON_READ_NOFLAG);
}

static int watch(const struct timao_request *request,
                 struct timao_reader *reader) {
  for (;;) {
    size_t length = 0;
    char *line = read_line(reader, &length);
    if (line == NULL)
      return lost();
    yyjson_doc *doc = yyjson_read(line, length, YYJSON_READ_NOFLAG);
    yyjson_val *event = yyjson_doc_get_root(doc);
    const char *kind = yyjson_get_str(yyjson_obj_get(event, "event"));
    int status = -1;
    if (kind == NULL)
      status = complain("protocol", "malformed event", 1);
    else if (strcmp(kind, "error") == 0) {
      (void)forward_error(yyjson_obj_get(event, "error"));
      status = 1;
    } else if (request->format == TIMAO_FORMAT_JSON) {
      line[length] = '\n';
      if (fwrite(line, 1, length + 1, stdout) != length + 1 ||
          fflush(stdout) != 0)
        status = complain("output", "cannot write to stdout", 1);
    } else if (strcmp(kind, "suspended") != 0 &&
               print(request->format, yyjson_obj_get(event, "value")) != 0)
      status = 1;
    yyjson_doc_free(doc);
    if (status >= 0)
      return status;
  }
}

static int run(struct timao_request *request, struct timao_reader *reader) {
  static const char hello[] = "{\"version\":1,\"id\":\"h\",\"op\":\"hello\"}\n";
  if (!send_all(reader->fd, hello, sizeof(hello) - 1))
    return lost();
  yyjson_doc *doc = read_message(reader);
  yyjson_val *reply = yyjson_doc_get_root(doc);
  if (reply == NULL) {
    yyjson_doc_free(doc);
    return lost();
  }
  if (!yyjson_get_bool(yyjson_obj_get(reply, "ok"))) {
    const int status = forward_error(yyjson_obj_get(reply, "error"));
    yyjson_doc_free(doc);
    return status;
  }
  const char *instance = yyjson_get_str(yyjson_obj_get(reply, "instance"));
  size_t length = 0;
  char *line = instance == NULL
                   ? NULL
                   : timao_request_line(request, "r", instance, &length);
  yyjson_doc_free(doc);
  if (line == NULL)
    return complain(
        instance == NULL ? "protocol" : "memory",
        instance == NULL ? "hello reply has no instance" : "out of memory", 1);
  const bool sent = send_all(reader->fd, line, length);
  free(line);
  if (!sent)
    return lost();

  doc = read_message(reader);
  reply = yyjson_doc_get_root(doc);
  int status = 0;
  if (reply == NULL)
    status = lost();
  else if (!yyjson_get_bool(yyjson_obj_get(reply, "ok")))
    status = forward_error(yyjson_obj_get(reply, "error"));
  else if (!request->watch)
    status = print(request->format, yyjson_obj_get(reply, "value"));
  yyjson_doc_free(doc);
  if (status != 0 || !request->watch)
    return status;
  return watch(request, reader);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    (void)fputs(help, stderr);
    return 2;
  }
  struct timao_request request = {0};
  const char *usage = NULL;
  if (!timao_request_parse(argc, argv, &request, &usage))
    return complain("usage", usage, 2);
  if (request.action != TIMAO_ACTION_RUN) {
    const char *text =
        request.action == TIMAO_ACTION_HELP ? help : "timao " LEME_VERSION "\n";
    return fputs(text, stdout) == EOF || fflush(stdout) != 0 ? 1 : 0;
  }
  const struct timao_endpoint_options options = {
      .explicit_path = request.socket,
      .environment_path = getenv("LEME_SOCKET"),
      .runtime_dir = getenv("XDG_RUNTIME_DIR"),
      .display = getenv("WAYLAND_DISPLAY")};
  struct timao_endpoint endpoint = {0};
  struct timao_reader reader = {.fd = -1};
  int status =
      timao_endpoint_resolve(&options, &endpoint)
          ? connect_socket(&endpoint, &reader.fd)
          : complain("connection",
                     "no socket, use --socket, LEME_SOCKET or WAYLAND_DISPLAY",
                     2);
  if (status == 0)
    status = run(&request, &reader);
  if (reader.fd >= 0)
    (void)close(reader.fd);
  free(reader.data);
  timao_request_finish(&request);
  return status;
}
