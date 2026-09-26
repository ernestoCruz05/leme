#include "timao/function.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"

static struct timao_heap_object *function_edge(const void *data, size_t index) {
  (void)index;
  const struct timao_value *value = data;
  return value->as.function.captured == NULL
             ? NULL
             : value->as.function.captured->allocation;
}
static void function_destroy(void *data) {
  const struct timao_value *value = data;
  timao_program_unref(value->as.function.program);
}
enum timao_status timao_function_create(struct timao_vm *vm,
                                        struct timao_program *program,
                                        uint32_t definition,
                                        const struct timao_value **out,
                                        struct timao_diagnostic *error) {
  *out = NULL;
  const uint32_t name = timao_child(program, definition, 1);
  const uint32_t parameters = timao_child(program, definition, 2);
  struct timao_environment *captured = NULL;
  if (timao_environment_replace(vm, vm->globals, program->nodes[name].text,
                                NULL, &captured, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_heap_object *allocation = NULL;
  if (timao_heap_alloc(&vm->heap, sizeof(struct timao_value), 1, function_edge,
                       function_destroy, &allocation, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_value *value = timao_heap_data(allocation);
  value->allocation = allocation;
  value->kind = TIMAO_CALLABLE;
  value->as.function.program = program;
  value->as.function.captured = captured;
  value->as.function.definition = definition;
  value->as.function.arity = program->nodes[parameters].count;
  timao_program_ref(program);
  *out = value;
  return TIMAO_OK;
}
enum timao_status timao_function_scope(
    struct timao_execution *execution, const struct timao_value *callable,
    const struct timao_value *const *arguments, size_t count,
    struct timao_environment **out, struct timao_diagnostic *error) {
  *out = NULL;
  if (callable == NULL || callable->kind != TIMAO_CALLABLE ||
      !timao_heap_owns(&execution->vm->heap, callable->allocation))
    return timao_error(error, "type_error", "local callable required");
  if (count != callable->as.function.arity)
    return timao_error(error, "arity_error", "wrong function argument count");
  struct timao_program *program = callable->as.function.program;
  const uint32_t name =
      timao_child(program, callable->as.function.definition, 1);
  const struct timao_member self = {.key = program->nodes[name].text,
                                    .value = callable};
  struct timao_environment *outer = NULL;
  if (timao_environment_create(execution->vm, callable->as.function.captured,
                               &self, 1, &outer, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (count == 0) {
    *out = outer;
    return TIMAO_OK;
  }
  struct timao_member *bindings = timao_memory_alloc(
      execution->vm->heap.account, count * sizeof(*bindings), error);
  if (bindings == NULL)
    return TIMAO_ERROR;
  uint32_t parameter =
      program->nodes[timao_child(program, callable->as.function.definition, 2)]
          .first;
  for (size_t i = 0; i < count; ++i) {
    if (arguments[i] == NULL ||
        !timao_heap_owns(&execution->vm->heap, arguments[i]->allocation)) {
      timao_memory_free(bindings);
      return timao_error(error, "invalid_argument",
                         "foreign function argument");
    }
    bindings[i] = (struct timao_member){.key = program->nodes[parameter].text,
                                        .value = arguments[i]};
    parameter = program->nodes[parameter].next;
  }
  const enum timao_status status = timao_environment_create(
      execution->vm, outer, bindings, count, out, error);
  timao_memory_free(bindings);
  return status;
}
