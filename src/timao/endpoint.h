#ifndef TIMAO_ENDPOINT_H
#define TIMAO_ENDPOINT_H

#include "public/budget.h"
#include <sys/socket.h>
#include <sys/un.h>

struct timao_endpoint_options {
  const char *explicit_path;
  const char *environment_path;
  const char *runtime_dir;
  const char *display;
};

struct timao_endpoint {
  struct sockaddr_un address;
  socklen_t length;
};

enum leme_public_status
timao_endpoint_resolve(const struct timao_endpoint_options *options,
                       struct timao_endpoint *out);

#endif
