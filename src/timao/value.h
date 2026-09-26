#ifndef TIMAO_VALUE_H
#define TIMAO_VALUE_H

#include "timao/language.h"

enum timao_value_kind {
  TIMAO_NULL,
  TIMAO_BOOLEAN,
  TIMAO_NUMBER,
  TIMAO_STRING,
  TIMAO_ARRAY,
  TIMAO_OBJECT,
  TIMAO_CALLABLE,
  TIMAO_WATCH
};

struct timao_member {
  struct leme_public_text key;
  const struct timao_value *value;
};

enum timao_value_kind timao_value_kind(const struct timao_value *value);
size_t timao_value_length(const struct timao_value *value);
enum timao_status timao_value_as_boolean(const struct timao_value *value,
                                         bool *out);
enum timao_status timao_value_as_number(const struct timao_value *value,
                                        double *out);
enum timao_status timao_value_watch_token(const struct timao_value *value,
                                          uint64_t *out);
enum timao_status timao_value_callable_arity(const struct timao_value *value,
                                             size_t *out);
enum timao_status timao_value_member_at(const struct timao_value *value,
                                        size_t index, struct timao_member *out);
enum timao_status timao_value_null(struct timao_vm *vm,
                                   const struct timao_value **out,
                                   struct timao_diagnostic *error);
enum timao_status timao_value_boolean(struct timao_vm *vm, bool input,
                                      const struct timao_value **out,
                                      struct timao_diagnostic *error);
enum timao_status timao_value_number(struct timao_vm *vm, double input,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error);
enum timao_status timao_value_object(struct timao_vm *vm,
                                     const struct timao_member *members,
                                     size_t count,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error);
enum timao_status timao_watch_value(struct timao_execution *execution,
                                    uint64_t token,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error);
enum timao_status timao_import(struct timao_execution *execution,
                               const struct leme_public_value *input,
                               const struct timao_value **out,
                               struct timao_diagnostic *error);
enum timao_status timao_export(struct timao_execution *execution,
                               const struct timao_value *input,
                               struct leme_public_builder *builder,
                               struct leme_public_value **out,
                               struct timao_diagnostic *error);

enum timao_status timao_export_json(struct timao_vm *vm,
                                    const struct timao_value *input,
                                    struct leme_public_budget *account,
                                    struct leme_public_builder **owner,
                                    const struct leme_public_value **out,
                                    struct timao_diagnostic *error);

enum timao_status timao_value_string(struct timao_vm *vm,
                                     struct leme_public_text input,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error);
enum timao_status timao_value_array(struct timao_vm *vm,
                                    const struct timao_value *const *items,
                                    size_t count,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error);
enum timao_status timao_value_text(const struct timao_value *value,
                                   struct leme_public_text *out);
enum timao_status timao_value_at(const struct timao_value *value, size_t index,
                                 const struct timao_value **out);

#endif
