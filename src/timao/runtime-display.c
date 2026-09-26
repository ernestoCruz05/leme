#include "timao/runtime-internal.h"
#include "timao/value.h"
#include <inttypes.h>
#include <stdio.h>

enum timao_status timao_runtime_show(struct timao_runtime *runtime,
                                     const struct timao_value *value,
                                     struct timao_diagnostic *error) {
  const enum timao_value_kind kind = timao_value_kind(value);
  if (kind == TIMAO_WATCH || kind == TIMAO_CALLABLE) {
    char description[64] = {0};
    uint64_t id = 0;
    size_t arity = 0;
    int length = 0;
    if (kind == TIMAO_WATCH) {
      if (timao_value_watch_token(value, &id) != TIMAO_OK)
        return timao_error(error, "type_error", "invalid local watch handle");
      length =
          snprintf(description, sizeof(description), "watch:%" PRIu64 "\n", id);
    } else {
      if (timao_value_callable_arity(value, &arity) != TIMAO_OK)
        return timao_error(error, "type_error", "invalid local callable");
      length =
          snprintf(description, sizeof(description), "<function/%zu>\n", arity);
    }
    if (length < 0 || (size_t)length >= sizeof(description))
      return timao_error(error, "resource_limit",
                         "local description exceeds limit");
    const enum timao_status status = timao_runtime_text(
        runtime, (struct leme_public_text){description, (size_t)length}, error);
    if (status != TIMAO_OK || kind != TIMAO_WATCH)
      return status;
    return timao_runtime_default_watch(runtime, id, error);
  }
  struct leme_public_builder *owner = NULL;
  const struct leme_public_value *json = NULL;
  enum timao_status status = timao_export_json(
      runtime->vm, value, runtime->account, &owner, &json, error);
  if (status == TIMAO_OK)
    status = timao_runtime_emit(runtime, json, error);
  leme_public_builder_destroy(owner);
  return status;
}
