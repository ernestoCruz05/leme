#ifndef TIMAO_REQUEST_H
#define TIMAO_REQUEST_H

#include "yyjson.h"

#include <stdbool.h>
#include <stddef.h>

enum timao_format { TIMAO_FORMAT_JSON, TIMAO_FORMAT_RAW };

enum timao_action { TIMAO_ACTION_RUN, TIMAO_ACTION_HELP, TIMAO_ACTION_VERSION };

struct timao_request {
  enum timao_action action;
  enum timao_format format;
  const char *socket;
  bool watch;
  yyjson_mut_doc *body;
};

bool timao_request_parse(int argc, char **argv, struct timao_request *out,
                         const char **usage);
char *timao_request_line(struct timao_request *request, const char *id,
                         const char *instance, size_t *length);
void timao_request_finish(struct timao_request *request);

#endif
