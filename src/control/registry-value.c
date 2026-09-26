#include "control/registry.h"

enum leme_public_status
leme_control_registry_value(struct leme_public_builder *builder,
                            struct leme_public_value **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (builder == NULL)
    return LEME_PUBLIC_INVALID;
  const struct leme_control_registry *registry = leme_control_registry();
  struct leme_public_value *array = NULL;
  enum leme_public_status status =
      leme_public_array(builder, registry->count, &array);
  if (status != LEME_PUBLIC_OK)
    return status;
  for (size_t i = 0; i < registry->count; ++i) {
    struct leme_public_value *val = NULL;
    status =
        leme_public_operation_value(builder, &registry->operations[i], &val);
    if (status != LEME_PUBLIC_OK)
      return status;
    status = leme_public_array_set(builder, array, i, val);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
