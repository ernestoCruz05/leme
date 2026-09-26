#include "timao/language-internal.h"
#include "timao/diagnostic.h"

struct timao_pin {
  struct timao_pin *next;
  struct timao_heap_root root;
  uint64_t id;
};

static enum timao_status copy_args(struct timao_vm *vm,
                                   const struct leme_public_value *args,
                                   struct timao_diagnostic *error) {
  const size_t count = args == NULL ? 0 : leme_public_length(args);
  if (count > SIZE_MAX / sizeof(const struct timao_value *))
    return timao_error(error, "resource_limit", "argv size overflow");
  if (count == 0)
    return timao_value_array(vm, NULL, 0, &vm->args, error);
  const struct timao_value **items =
      (const struct timao_value **)timao_memory_alloc(
          vm->heap.account, count * sizeof(*items), error);
  if (items == NULL)
    return TIMAO_ERROR;
  enum timao_status status = TIMAO_ERROR;
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_text text = {0};
    if (leme_public_as_text(leme_public_at(args, i), &text) != LEME_PUBLIC_OK) {
      timao_error(error, "type_error", "argv elements must be strings");
      goto done;
    }
    if (timao_value_string(vm, text, &items[i], error) != TIMAO_OK)
      goto done;
  }
  status = timao_value_array(vm, items, count, &vm->args, error);
done:
  timao_memory_free((void *)items);
  return status;
}

enum timao_status timao_vm_create(struct leme_public_budget *account,
                                  const struct timao_limits *limits,
                                  const struct timao_host *host,
                                  const struct leme_public_value *args,
                                  struct timao_vm **out,
                                  struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing VM output");
  *out = NULL;
  if (account == NULL)
    return timao_error(error, "invalid_argument",
                       "missing interpreter account");
  if (args != NULL && leme_public_kind(args) != LEME_PUBLIC_ARRAY)
    return timao_error(error, "type_error", "argv must be an array of strings");
  struct timao_limits selected = {0};
  if (timao_limits_resolve(limits, &selected, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct leme_public_budget *child = NULL;
  const enum leme_public_status status =
      leme_public_budget_child(account, selected.memory_bytes, &child);
  if (status != LEME_PUBLIC_OK)
    return timao_error(
        error, status == LEME_PUBLIC_OOM ? "out_of_memory" : "resource_limit",
        "unable to create VM account");
  struct timao_vm *vm = timao_memory_alloc(child, sizeof(*vm), error);
  if (vm == NULL) {
    leme_public_budget_unref(child);
    return TIMAO_ERROR;
  }
  vm->heap.account = child;
  vm->heap.maximum = selected.memory_bytes;
  vm->limits = selected;
  vm->host = host == NULL ? (struct timao_host){0} : *host;
  vm->next_pin = 1;
  timao_meter_init(&vm->meter, selected.steps, vm->host.context,
                   vm->host.cancelled);
  if (copy_args(vm, args, error) != TIMAO_OK) {
    timao_vm_destroy(vm);
    return TIMAO_ERROR;
  }
  timao_heap_root_add(&vm->heap, &vm->args_root, vm->args->allocation);
  *out = vm;
  return TIMAO_OK;
}

void timao_vm_destroy(struct timao_vm *vm) {
  if (vm == NULL)
    return;
  while (vm->pins != NULL)
    timao_unpin(vm, vm->pins->id);
  timao_program_unref(vm->file_program);
  timao_heap_finish(&vm->heap);
  struct leme_public_budget *account = vm->heap.account;
  timao_memory_free(vm);
  leme_public_budget_unref(account);
}

enum timao_status timao_pin(struct timao_vm *vm,
                            const struct timao_value *value, uint64_t *root,
                            struct timao_diagnostic *error) {
  if (root == NULL)
    return timao_error(error, "invalid_argument", "missing root output");
  *root = 0;
  if (vm == NULL || value == NULL ||
      !timao_heap_owns(&vm->heap, value->allocation))
    return timao_error(error, "invalid_argument", "foreign pinned value");
  if (vm->next_pin == 0)
    return timao_error(error, "resource_limit", "root identifiers exhausted");
  struct timao_pin *pin =
      timao_memory_alloc(vm->heap.account, sizeof(*pin), error);
  if (pin == NULL)
    return TIMAO_ERROR;
  pin->id = vm->next_pin++;
  pin->next = vm->pins;
  vm->pins = pin;
  timao_heap_root_add(&vm->heap, &pin->root, value->allocation);
  *root = pin->id;
  return TIMAO_OK;
}

void timao_unpin(struct timao_vm *vm, uint64_t root) {
  if (vm == NULL || root == 0)
    return;
  struct timao_pin **slot = &vm->pins;
  while (*slot != NULL && (*slot)->id != root)
    slot = &(*slot)->next;
  if (*slot == NULL)
    return;
  struct timao_pin *pin = *slot;
  *slot = pin->next;
  timao_heap_root_remove(&vm->heap, &pin->root);
  timao_memory_free(pin);
}
