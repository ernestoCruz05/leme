#ifndef TIMAO_LANGUAGE_H
#define TIMAO_LANGUAGE_H

#include "public/value.h"
#include <stdint.h>

struct leme_public_budget;
struct timao_vm;
struct timao_execution;
struct timao_value;
struct timao_diagnostic;
struct timao_host;
struct timao_limits;
struct timao_program;

enum timao_mode { TIMAO_FILE, TIMAO_EXPRESSION, TIMAO_REPL };
enum timao_status { TIMAO_OK, TIMAO_INCOMPLETE, TIMAO_ERROR };
enum timao_context { TIMAO_NORMAL, TIMAO_PURE };

struct timao_span {
  size_t begin;
  size_t end;
};

struct timao_input {
  struct leme_public_text name;
  struct leme_public_text bytes;
  enum timao_mode mode;
  void *cancel_context;
  bool (*cancelled)(void *context);
};

enum timao_status timao_prepare(struct leme_public_budget *account,
                                const struct timao_limits *limits,
                                const struct timao_input *input,
                                struct timao_program **out,
                                struct timao_diagnostic *error);
void timao_program_ref(struct timao_program *program);
void timao_program_unref(struct timao_program *program);
size_t timao_program_forms(const struct timao_program *program);

enum timao_status timao_vm_create(struct leme_public_budget *account,
                                  const struct timao_limits *limits,
                                  const struct timao_host *host,
                                  const struct leme_public_value *args,
                                  struct timao_vm **out,
                                  struct timao_diagnostic *error);
void timao_vm_destroy(struct timao_vm *vm);
enum timao_status timao_eval_form(struct timao_vm *vm,
                                  struct timao_program *program, size_t index,
                                  const struct timao_value **out,
                                  struct timao_diagnostic *error);
enum timao_status timao_invoke_handler(struct timao_vm *vm,
                                       const struct timao_value *callable,
                                       const struct timao_value *event,
                                       const struct timao_value **out,
                                       struct timao_diagnostic *error);
enum timao_status timao_invoke_handler_json(
    struct timao_vm *vm, const struct timao_value *callable,
    const struct leme_public_value *event, const struct timao_value **out,
    struct timao_diagnostic *error);
enum timao_status timao_call(struct timao_execution *execution,
                             const struct timao_value *callable,
                             const struct timao_value *const *args,
                             size_t count, enum timao_context context,
                             const struct timao_value **out,
                             struct timao_diagnostic *error);
enum timao_status timao_pin(struct timao_vm *vm,
                            const struct timao_value *value, uint64_t *root,
                            struct timao_diagnostic *error);
void timao_unpin(struct timao_vm *vm, uint64_t root);

#endif
