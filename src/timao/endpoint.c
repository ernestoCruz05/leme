#include "timao/endpoint.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(struct sockaddr_un) <= UINT_MAX, "socket address size");

static enum leme_public_status
resolve(const struct timao_endpoint_options *options,
        struct timao_endpoint *candidate) {
  if (options == NULL)
    return LEME_PUBLIC_INVALID;
  const char *selected = options->explicit_path;
  if (selected != NULL && selected[0] == '\0')
    return LEME_PUBLIC_INVALID;
  if (selected == NULL && options->environment_path != NULL &&
      options->environment_path[0] != '\0')
    selected = options->environment_path;
  const char *parts[4] = {selected, NULL, NULL, NULL};
  if (selected == NULL) {
    if (options->runtime_dir == NULL || options->runtime_dir[0] == '\0' ||
        options->display == NULL || options->display[0] == '\0')
      return LEME_PUBLIC_UNAVAILABLE;
    parts[0] = options->runtime_dir;
    parts[1] = "/leme-";
    parts[2] = options->display;
    parts[3] = ".sock";
  }
  size_t length = 0;
  const size_t capacity = sizeof(candidate->address.sun_path);
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
    if (parts[i] == NULL)
      continue;
    const size_t remaining = capacity - length;
    const size_t part_length = strnlen(parts[i], remaining);
    if (part_length == remaining)
      return LEME_PUBLIC_LIMIT;
    memcpy(candidate->address.sun_path + length, parts[i], part_length);
    length += part_length;
  }
  candidate->address.sun_path[length] = '\0';
  candidate->address.sun_family = AF_UNIX;
  candidate->length =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
  return LEME_PUBLIC_OK;
}

enum leme_public_status
timao_endpoint_resolve(const struct timao_endpoint_options *options,
                       struct timao_endpoint *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  struct timao_endpoint candidate = {0};
  const enum leme_public_status status = resolve(options, &candidate);
  *out = status == LEME_PUBLIC_OK ? candidate : (struct timao_endpoint){0};
  return status;
}
