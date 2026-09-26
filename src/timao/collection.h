#ifndef TIMAO_COLLECTION_H
#define TIMAO_COLLECTION_H

#include "timao/parser.h"
#include "timao/value.h"

enum timao_status timao_local_field(struct timao_execution *execution,
                                    const struct timao_value *item,
                                    struct leme_public_text path,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error);
enum timao_status timao_collection_fields(struct timao_execution *execution,
                                          struct timao_program *program,
                                          uint32_t call,
                                          struct timao_diagnostic *error);
enum timao_status timao_project(struct timao_execution *execution,
                                struct timao_program *program, uint32_t call,
                                const struct timao_value *collection,
                                const struct timao_value **out,
                                struct timao_diagnostic *error);
enum timao_status timao_sort(struct timao_execution *execution,
                             const struct timao_value *collection,
                             struct leme_public_text path,
                             const struct timao_value *direction,
                             const struct timao_value **out,
                             struct timao_diagnostic *error);

#endif
