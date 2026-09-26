#ifndef TIMAO_BINDINGS_H
#define TIMAO_BINDINGS_H

#include "timao/environment.h"
#include "timao/parser.h"
#include "timao/host.h"

bool timao_host_operation(struct leme_public_text name,
                          enum timao_host_op *out);
enum timao_status timao_host_local(struct timao_execution *execution,
                                   enum timao_host_op operation,
                                   const struct timao_value *const *arguments,
                                   size_t count, const struct timao_value **out,
                                   struct timao_diagnostic *error);
enum timao_status timao_host_request(struct timao_execution *execution,
                                     struct timao_program *program,
                                     uint32_t expression,
                                     struct timao_environment *environment,
                                     enum timao_host_op operation,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error);

#endif
