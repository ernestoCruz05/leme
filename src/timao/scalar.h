#ifndef TIMAO_SCALAR_H
#define TIMAO_SCALAR_H

#include "timao/language.h"
#include "control/registry.h"

enum timao_status timao_text_order(struct timao_vm *vm,
                                   struct leme_public_text left,
                                   struct leme_public_text right, int *out,
                                   struct timao_diagnostic *error);
enum timao_status timao_equal(struct timao_vm *vm,
                              const struct timao_value *left,
                              const struct timao_value *right, bool *out,
                              struct timao_diagnostic *error);
enum timao_status timao_get(struct timao_vm *vm,
                            const struct timao_value *object,
                            struct leme_public_text key,
                            const struct timao_value **out,
                            struct timao_diagnostic *error);
enum timao_status timao_scalar(struct timao_execution *execution,
                               const struct leme_control_operator *op,
                               const struct timao_value *const *args,
                               size_t count, const struct timao_value **out,
                               struct timao_diagnostic *error);

#endif
