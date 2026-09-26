#include "timao/bindings.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include <string.h>

struct timao_vm *timao_execution_vm(const struct timao_execution *execution) {
  return execution == NULL ? NULL : execution->vm;
}
struct leme_public_budget *
timao_execution_account(const struct timao_execution *execution) {
  return execution == NULL || execution->vm == NULL
             ? NULL
             : execution->vm->heap.account;
}
enum timao_context
timao_execution_context(const struct timao_execution *execution) {
  return execution == NULL ? TIMAO_PURE : execution->context;
}
enum timao_status timao_execution_charge(struct timao_execution *execution,
                                         size_t units,
                                         struct timao_diagnostic *error) {
  if (execution == NULL || execution->vm == NULL || !execution->vm->active)
    return timao_error(error, "invalid_argument", "inactive execution context");
  return timao_charge(&execution->vm->meter, units, error);
}

bool timao_host_operation(struct leme_public_text name,
                          enum timao_host_op *out) {
  static const struct {
    const char *name;
    enum timao_host_op operation;
  } entries[] = {{"query", TIMAO_HOST_QUERY},   {"act", TIMAO_HOST_ACT},
                 {"watch", TIMAO_HOST_WATCH},   {"on", TIMAO_HOST_ON},
                 {"cancel", TIMAO_HOST_CANCEL}, {"await", TIMAO_HOST_AWAIT},
                 {"emit", TIMAO_HOST_EMIT},     {"launch", TIMAO_HOST_LAUNCH}};
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
    if (strlen(entries[i].name) == name.length &&
        memcmp(entries[i].name, name.data, name.length) == 0) {
      if (out != NULL)
        *out = entries[i].operation;
      return true;
    }
  return false;
}
