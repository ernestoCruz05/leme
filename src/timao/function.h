#ifndef TIMAO_FUNCTION_H
#define TIMAO_FUNCTION_H

#include "timao/environment.h"
#include "timao/parser.h"

enum timao_status timao_function_create(struct timao_vm *vm,
                                        struct timao_program *program,
                                        uint32_t definition,
                                        const struct timao_value **out,
                                        struct timao_diagnostic *error);
enum timao_status timao_function_scope(
    struct timao_execution *execution, const struct timao_value *callable,
    const struct timao_value *const *arguments, size_t count,
    struct timao_environment **out, struct timao_diagnostic *error);

#endif
