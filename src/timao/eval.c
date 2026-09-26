#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include "control/registry.h"
#include "timao/function.h"
#include "timao/scalar.h"
#include "timao/collection.h"
#include "timao/bindings.h"
#include <string.h>

enum frame_state {
  FRAME_INIT,
  FRAME_SEQUENCE,
  FRAME_RETURN,
  FRAME_IF,
  FRAME_DEF,
  FRAME_LET,
  FRAME_TRY,
  FRAME_FOR_COLLECTION,
  FRAME_FOR_BODY,
  FRAME_ARGUMENTS,
  FRAME_LOGIC,
  FRAME_COLLECTION_START,
  FRAME_COLLECTION_ITEM,
  FRAME_SORT_ORDER
};
struct frame {
  struct timao_heap_object *allocation;
  struct timao_heap_root root;
  struct frame *parent;
  struct timao_program *program;
  struct timao_environment *environment;
  struct timao_environment *outer;
  const struct timao_value *item;
  const struct timao_value *result;
  const struct timao_value *collection;
  const struct timao_value *callee;
  struct timao_value *output;
  const struct timao_value **values;
  size_t capacity;
  size_t used;
  size_t iteration;
  uint32_t node;
  uint32_t cursor;
  enum frame_state state;
  bool waiting;
  bool function_entry;
};

static struct timao_heap_object *frame_edge(const void *data, size_t index) {
  const struct frame *frame = data;
  if (index == 0)
    return frame->environment == NULL ? NULL : frame->environment->allocation;
  if (index == 1)
    return frame->outer == NULL ? NULL : frame->outer->allocation;
  const struct timao_value *value = index == 2   ? frame->item
                                    : index == 3 ? frame->result
                                    : index == 4 ? frame->collection
                                    : index == 5 ? frame->callee
                                    : index == 6 ? frame->output
                                                 : frame->values[index - 7];
  return value == NULL ? NULL : value->allocation;
}
static void frame_destroy(void *data) {
  const struct frame *frame = data;
  timao_program_unref(frame->program);
}
static struct frame *push(struct timao_execution *execution,
                          struct frame *parent, struct timao_program *program,
                          uint32_t node, struct timao_environment *environment,
                          const struct timao_value *item,
                          struct timao_diagnostic *error) {
  const struct timao_node *syntax = timao_node_at(program, node);
  size_t capacity = syntax == NULL ? 0 : syntax->count;
  if (syntax != NULL && syntax->kind == TIMAO_TOKEN_OPEN &&
      timao_node_is(timao_node_at(program, syntax->first), "let")) {
    const struct timao_node *bindings =
        timao_node_at(program, timao_child(program, node, 1));
    if (bindings != NULL && bindings->count > capacity)
      capacity = bindings->count;
  }
  if (capacity >
      (SIZE_MAX - sizeof(struct frame)) / sizeof(const struct timao_value *)) {
    timao_error(error, "resource_limit", "evaluation frame overflow");
    return NULL;
  }
  struct timao_heap_object *allocation = NULL;
  if (timao_heap_alloc(&execution->vm->heap,
                       sizeof(struct frame) +
                           capacity * sizeof(const struct timao_value *),
                       capacity + 7, frame_edge, frame_destroy, &allocation,
                       error) != TIMAO_OK)
    return NULL;
  struct frame *frame = timao_heap_data(allocation);
  frame->allocation = allocation;
  frame->parent = parent;
  frame->program = program;
  frame->node = node;
  frame->environment = environment;
  frame->outer = environment;
  frame->item = item;
  frame->capacity = capacity;
  frame->values = (const struct timao_value **)(frame + 1);
  timao_program_ref(program);
  timao_heap_root_add(&execution->vm->heap, &frame->root, allocation);
  return frame;
}
static enum timao_status start_child(struct timao_execution *execution,
                                     struct frame **stack, uint32_t node,
                                     struct timao_diagnostic *error) {
  struct frame *parent = *stack;
  struct frame *child = push(execution, parent, parent->program, node,
                             parent->environment, parent->item, error);
  if (child == NULL)
    return TIMAO_ERROR;
  *stack = child;
  return TIMAO_OK;
}
static void pop(struct timao_execution *execution, struct frame **stack,
                const struct timao_value *value) {
  struct frame *frame = *stack;
  *stack = frame->parent;
  if (*stack != NULL)
    (*stack)->result = value;
  if (frame->function_entry)
    --execution->call_depth;
  timao_heap_root_remove(&execution->vm->heap, &frame->root);
}
static void publish_result(struct timao_vm *vm,
                           const struct timao_value *value) {
  timao_heap_root_remove(&vm->heap, &vm->result_root);
  timao_heap_root_add(&vm->heap, &vm->result_root, value->allocation);
}
static void publish_globals(struct timao_vm *vm,
                            struct timao_environment *environment) {
  if (vm->globals != NULL)
    timao_heap_root_remove(&vm->heap, &vm->globals_root);
  vm->globals = environment;
  timao_heap_root_add(&vm->heap, &vm->globals_root, environment->allocation);
}
enum timao_status timao_local_field(struct timao_execution *execution,
                                    const struct timao_value *item,
                                    struct leme_public_text path,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error) {
  const struct timao_value *value = item;
  size_t start = 1;
  for (size_t i = 1; i <= path.length; ++i) {
    if (i % 128 == 1 &&
        timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    if (i != path.length && path.data[i] != '.')
      continue;
    if (value == NULL || value->kind != TIMAO_OBJECT)
      return timao_error(error, "type_error",
                         "field traversal requires a local object");
    const struct leme_public_text key = {path.data + start, i - start};
    const struct timao_value *found = NULL;
    if (timao_get(execution->vm, value, key, &found, error) != TIMAO_OK)
      return TIMAO_ERROR;
    value = found;
    start = i + 1;
  }
  *out = value;
  return TIMAO_OK;
}
static enum timao_status bind_let(struct timao_execution *execution,
                                  struct frame *frame,
                                  struct timao_diagnostic *error) {
  const uint32_t list = timao_child(frame->program, frame->node, 1);
  const size_t count = frame->program->nodes[list].count;
  if (count == 0) {
    frame->cursor = frame->program->nodes[list].next;
    frame->state = FRAME_SEQUENCE;
    return TIMAO_OK;
  }
  struct timao_member *bindings = timao_memory_alloc(
      execution->vm->heap.account, count * sizeof(*bindings), error);
  if (bindings == NULL)
    return TIMAO_ERROR;
  uint32_t entry = frame->program->nodes[list].first;
  for (size_t i = 0; i < count; ++i) {
    bindings[i] = (struct timao_member){
        .key = frame->program->nodes[frame->program->nodes[entry].first].text,
        .value = frame->values[i]};
    entry = frame->program->nodes[entry].next;
  }
  const enum timao_status status = timao_environment_create(
      execution->vm, frame->outer, bindings, count, &frame->environment, error);
  timao_memory_free(bindings);
  frame->cursor = frame->program->nodes[list].next;
  frame->state = FRAME_SEQUENCE;
  return status;
}
static enum timao_status begin_iteration(struct timao_execution *execution,
                                         struct frame *frame,
                                         struct timao_diagnostic *error) {
  const uint32_t name = timao_child(frame->program, frame->node, 1);
  const struct timao_member binding = {
      .key = frame->program->nodes[name].text,
      .value = frame->collection->as.array.items[frame->iteration]};
  if (timao_environment_create(execution->vm, frame->outer, &binding, 1,
                               &frame->environment, error) != TIMAO_OK)
    return TIMAO_ERROR;
  frame->cursor = timao_child(frame->program, frame->node, 3);
  frame->state = FRAME_FOR_BODY;
  return TIMAO_OK;
}
static enum timao_status catch_error(struct timao_execution *execution,
                                     struct frame **stack,
                                     struct timao_diagnostic *error) {
  if (execution->vm->meter.failure != NULL)
    return TIMAO_ERROR;
  while (*stack != NULL && (*stack)->state != FRAME_TRY)
    pop(execution, stack, NULL);
  if (*stack == NULL)
    return TIMAO_ERROR;
  struct frame *frame = *stack;
  const uint32_t catcher = timao_child(frame->program, frame->node, 2);
  const uint32_t name = timao_child(frame->program, catcher, 1);
  struct timao_diagnostic original = *error;
  *error = (struct timao_diagnostic){0};
  const struct timao_value *record = NULL;
  if (timao_diagnostic_value(execution, &original, &record, error) != TIMAO_OK)
    goto failed;
  const struct timao_member binding = {.key = frame->program->nodes[name].text,
                                       .value = record};
  if (timao_environment_create(execution->vm, frame->outer, &binding, 1,
                               &frame->environment, error) != TIMAO_OK)
    goto failed;
  frame->state = FRAME_SEQUENCE;
  frame->cursor = timao_child(frame->program, catcher, 2);
  frame->result = NULL;
  timao_diagnostic_destroy(&original);
  timao_diagnostic_destroy(error);
  return TIMAO_OK;
failed:
  timao_diagnostic_destroy(error);
  *error = original;
  return TIMAO_ERROR;
}
static struct frame *start_function(struct timao_execution *execution,
                                    struct frame *parent,
                                    const struct timao_value *callable,
                                    const struct timao_value *const *args,
                                    size_t count,
                                    struct timao_diagnostic *error) {
  if (execution->call_depth >= execution->vm->limits.call_depth) {
    timao_error(error, "resource_limit", "function call depth exceeded");
    return NULL;
  }
  struct timao_environment *environment = NULL;
  if (timao_function_scope(execution, callable, args, count, &environment,
                           error) != TIMAO_OK)
    return NULL;
  struct frame *frame = push(execution, parent, callable->as.function.program,
                             0, environment, NULL, error);
  if (frame == NULL)
    return NULL;
  frame->state = FRAME_SEQUENCE;
  frame->cursor =
      timao_child(frame->program, callable->as.function.definition, 3);
  frame->function_entry = true;
  ++execution->call_depth;
  return frame;
}
static bool host_name(const struct timao_node *head) {
  return head != NULL && timao_host_operation(head->text, NULL);
}
static void diagnose_stack(struct frame *stack,
                           struct timao_diagnostic *error) {
  for (struct frame *frame = stack; frame != NULL; frame = frame->parent) {
    const struct timao_node *node = timao_node_at(frame->program, frame->node);
    if (node == NULL)
      continue;
    if (error->source == NULL)
      timao_diagnostic_at(error, frame->program->source,
                          error->span.end != 0 ? error->span : node->span);
    if (frame->callee != NULL) {
      const struct timao_node *head =
          timao_node_at(frame->program, node->first);
      timao_diagnostic_call(error, frame->program->source, node->span,
                            head->span);
    }
  }
}
static enum timao_status run(struct timao_execution *execution,
                             struct frame *stack,
                             const struct timao_value **out,
                             struct timao_diagnostic *error) {
  struct timao_vm *vm = execution->vm;
  size_t since_gc = 0;
  enum timao_status status = TIMAO_OK;
  while (stack != NULL) {
    struct frame *frame = stack;
    const struct timao_node *node = timao_node_at(frame->program, frame->node);
    const struct timao_value *result = NULL;
    bool complete = false;
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK) {
      status = TIMAO_ERROR;
      break;
    }
    switch (frame->state) {
    case FRAME_INIT:
      switch (node->kind) {
      case TIMAO_TOKEN_NULL:
        status = timao_value_null(vm, &result, error);
        complete = true;
        break;
      case TIMAO_TOKEN_TRUE:
      case TIMAO_TOKEN_FALSE:
        status = timao_value_boolean(vm, node->kind == TIMAO_TOKEN_TRUE,
                                     &result, error);
        complete = true;
        break;
      case TIMAO_TOKEN_NUMBER:
        status = timao_value_number(vm, node->number, &result, error);
        complete = true;
        break;
      case TIMAO_TOKEN_STRING:
        status = timao_value_string(vm, node->text, &result, error);
        complete = true;
        break;
      case TIMAO_TOKEN_NAME:
        status = timao_environment_get(vm, frame->environment, node->text,
                                       &result, error);
        complete = true;
        break;
      case TIMAO_TOKEN_FIELD:
        status = timao_local_field(execution, frame->item, node->text, &result,
                                   error);
        complete = true;
        break;
      case TIMAO_TOKEN_OPEN: {
        const struct timao_node *head =
            timao_node_at(frame->program, node->first);
        if (execution->context == TIMAO_PURE && host_name(head)) {
          status = timao_error(error, "invalid_argument",
                               "host operations are forbidden in pure calls");
          break;
        }
        if (timao_node_is(head, "do")) {
          frame->state = FRAME_SEQUENCE;
          frame->cursor = head->next;
        } else if (timao_node_is(head, "and") || timao_node_is(head, "or")) {
          frame->state = FRAME_LOGIC;
          frame->cursor = head->next;
        } else if (timao_node_is(head, "if")) {
          frame->state = FRAME_IF;
          status = start_child(execution, &stack, head->next, error);
        } else if (timao_node_is(head, "def")) {
          frame->state = FRAME_DEF;
          status =
              start_child(execution, &stack,
                          timao_child(frame->program, frame->node, 2), error);
        } else if (timao_node_is(head, "defn")) {
          status = timao_value_null(vm, &result, error);
          if (status == TIMAO_OK)
            status = timao_function_create(vm, frame->program, frame->node,
                                           &frame->result, error);
          struct timao_environment *environment = NULL;
          const struct timao_node *name = timao_node_at(
              frame->program, timao_child(frame->program, frame->node, 1));
          if (status == TIMAO_OK)
            status =
                timao_environment_replace(vm, vm->globals, name->text,
                                          frame->result, &environment, error);
          if (status == TIMAO_OK)
            publish_globals(vm, environment);
          complete = true;
        } else if (timao_node_is(head, "let")) {
          frame->state = FRAME_LET;
          frame->cursor = frame->program->nodes[head->next].first;
        } else if (timao_node_is(head, "try")) {
          frame->state = FRAME_TRY;
          status = start_child(execution, &stack, head->next, error);
        } else if (timao_node_is(head, "for-each")) {
          frame->state = FRAME_FOR_COLLECTION;
          status =
              start_child(execution, &stack,
                          timao_child(frame->program, frame->node, 2), error);
        } else if (timao_node_is(head, "query") || timao_node_is(head, "act") ||
                   timao_node_is(head, "watch")) {
          const enum timao_host_op operation =
              timao_node_is(head, "query") ? TIMAO_HOST_QUERY
              : timao_node_is(head, "act") ? TIMAO_HOST_ACT
                                           : TIMAO_HOST_WATCH;
          status =
              timao_host_request(execution, frame->program, head->next,
                                 frame->environment, operation, &result, error);
          complete = true;
        } else if (timao_node_is(head, "where") || timao_node_is(head, "map") ||
                   timao_node_is(head, "select") ||
                   timao_node_is(head, "sort-by")) {
          if (timao_node_is(head, "select") || timao_node_is(head, "sort-by"))
            status = timao_collection_fields(execution, frame->program,
                                             frame->node, error);
          if (status == TIMAO_OK) {
            frame->state = FRAME_COLLECTION_START;
            status = start_child(execution, &stack, head->next, error);
          }
        } else {
          frame->state = FRAME_ARGUMENTS;
          frame->cursor = head->next;
          if (leme_control_operator_find(head->text) == NULL &&
              !host_name(head)) {
            status = timao_environment_get(vm, frame->environment, head->text,
                                           &frame->callee, error);
            if (status == TIMAO_OK && frame->callee->kind != TIMAO_CALLABLE)
              status = timao_error(error, "type_error",
                                   "function position requires a callable");
            if (status == TIMAO_OK &&
                frame->callee->as.function.arity != node->count - 1)
              status = timao_error(error, "arity_error",
                                   "wrong function argument count");
          }
        }
        break;
      }
      default:
        status = timao_error(error, "syntax_error", "invalid evaluation node");
        break;
      }
      break;
    case FRAME_SEQUENCE:
      if (frame->cursor == 0) {
        result = frame->result;
        if (result == NULL)
          status = timao_value_null(vm, &result, error);
        complete = true;
      } else {
        const uint32_t child = frame->cursor;
        frame->cursor = frame->program->nodes[child].next;
        status = start_child(execution, &stack, child, error);
      }
      break;
    case FRAME_RETURN:
    case FRAME_TRY:
      result = frame->result;
      complete = true;
      break;
    case FRAME_IF:
      if (frame->result->kind != TIMAO_BOOLEAN)
        status =
            timao_error(error, "type_error", "if condition must be boolean");
      else {
        const uint32_t branch = timao_child(frame->program, frame->node,
                                            frame->result->as.boolean ? 2 : 3);
        frame->state = FRAME_RETURN;
        status = start_child(execution, &stack, branch, error);
      }
      break;
    case FRAME_DEF: {
      const struct timao_node *name = timao_node_at(
          frame->program, timao_child(frame->program, frame->node, 1));
      struct timao_environment *environment = NULL;
      status = timao_value_null(vm, &result, error);
      if (status == TIMAO_OK)
        status = timao_environment_replace(vm, vm->globals, name->text,
                                           frame->result, &environment, error);
      if (status == TIMAO_OK)
        publish_globals(vm, environment);
      complete = true;
      break;
    }
    case FRAME_LET:
      if (frame->waiting) {
        frame->values[frame->used++] = frame->result;
        frame->waiting = false;
        frame->cursor = frame->program->nodes[frame->cursor].next;
      }
      if (frame->cursor == 0)
        status = bind_let(execution, frame, error);
      else {
        frame->waiting = true;
        status =
            start_child(execution, &stack,
                        timao_child(frame->program, frame->cursor, 1), error);
      }
      break;
    case FRAME_FOR_COLLECTION:
      if (frame->result->kind != TIMAO_ARRAY) {
        status = timao_error(error, "type_error", "for-each requires an array");
        break;
      }
      frame->collection = frame->result;
      if (frame->collection->as.array.count == 0) {
        status = timao_value_null(vm, &result, error);
        complete = true;
      } else
        status = begin_iteration(execution, frame, error);
      break;
    case FRAME_FOR_BODY:
      if (frame->cursor != 0) {
        const uint32_t child = frame->cursor;
        frame->cursor = frame->program->nodes[child].next;
        status = start_child(execution, &stack, child, error);
      } else if (++frame->iteration == frame->collection->as.array.count) {
        status = timao_value_null(vm, &result, error);
        complete = true;
      } else
        status = begin_iteration(execution, frame, error);
      break;
    case FRAME_COLLECTION_START: {
      if (frame->result->kind != TIMAO_ARRAY) {
        status = timao_error(error, "type_error",
                             "collection operator requires an array");
        break;
      }
      frame->collection = frame->result;
      const struct timao_node *head =
          timao_node_at(frame->program, node->first);
      if (timao_node_is(head, "select")) {
        status = timao_project(execution, frame->program, frame->node,
                               frame->collection, &result, error);
        complete = true;
      } else if (timao_node_is(head, "sort-by")) {
        frame->state = FRAME_SORT_ORDER;
        status =
            start_child(execution, &stack,
                        timao_child(frame->program, frame->node, 3), error);
      } else {
        status = timao_array_builder(vm, frame->collection->as.array.count,
                                     &frame->output, error);
        if (status != TIMAO_OK)
          break;
        if (frame->collection->as.array.count == 0) {
          result = frame->output;
          complete = true;
        } else {
          frame->state = FRAME_COLLECTION_ITEM;
          frame->item = frame->collection->as.array.items[0];
          status =
              start_child(execution, &stack,
                          timao_child(frame->program, frame->node, 2), error);
        }
      }
      break;
    }
    case FRAME_COLLECTION_ITEM: {
      const bool where =
          timao_node_is(timao_node_at(frame->program, node->first), "where");
      if (where && frame->result->kind != TIMAO_BOOLEAN) {
        status =
            timao_error(error, "type_error", "where predicate must be boolean");
        break;
      }
      if (!where || frame->result->as.boolean)
        frame->output->as.array.items[frame->used++] =
            where ? frame->item : frame->result;
      if (++frame->iteration == frame->collection->as.array.count) {
        frame->output->as.array.count = frame->used;
        result = frame->output;
        complete = true;
      } else {
        frame->item = frame->collection->as.array.items[frame->iteration];
        status =
            start_child(execution, &stack,
                        timao_child(frame->program, frame->node, 2), error);
      }
      break;
    }
    case FRAME_SORT_ORDER:
      status = timao_sort(
          execution, frame->collection,
          frame->program->nodes[timao_child(frame->program, frame->node, 2)]
              .text,
          frame->result, &result, error);
      complete = true;
      break;
    case FRAME_LOGIC: {
      const bool conjunction =
          timao_node_is(timao_node_at(frame->program, node->first), "and");
      if (frame->waiting) {
        frame->waiting = false;
        if (frame->result->kind != TIMAO_BOOLEAN) {
          status = timao_error(error, "type_error",
                               "logical operands must be booleans");
          break;
        }
        if (frame->result->as.boolean != conjunction) {
          result = frame->result;
          complete = true;
          break;
        }
      }
      if (frame->cursor == 0) {
        status = timao_value_boolean(vm, conjunction, &result, error);
        complete = true;
      } else {
        const uint32_t child = frame->cursor;
        frame->cursor = frame->program->nodes[child].next;
        frame->waiting = true;
        status = start_child(execution, &stack, child, error);
      }
      break;
    }
    case FRAME_ARGUMENTS:
      if (frame->waiting) {
        frame->values[frame->used++] = frame->result;
        frame->waiting = false;
      }
      if (frame->cursor != 0) {
        const uint32_t child = frame->cursor;
        frame->cursor = frame->program->nodes[child].next;
        frame->waiting = true;
        status = start_child(execution, &stack, child, error);
      } else {
        const struct timao_node *head =
            timao_node_at(frame->program, node->first);
        if (frame->callee != NULL) {
          struct frame *body =
              start_function(execution, frame, frame->callee, frame->values,
                             frame->used, error);
          if (body == NULL)
            status = TIMAO_ERROR;
          else {
            frame->state = FRAME_RETURN;
            stack = body;
          }
        } else {
          const struct leme_control_operator *op =
              leme_control_operator_find(head->text);
          if (op != NULL && op->opcode == LEME_CONTROL_OP_ITEM) {
            if (frame->item == NULL)
              status = timao_error(error, "type_error",
                                   "item outside collection scope");
            else
              result = frame->item;
          } else if (op != NULL)
            status = timao_scalar(execution, op, frame->values, frame->used,
                                  &result, error);
          else {
            enum timao_host_op operation = TIMAO_HOST_QUERY;
            if (timao_host_operation(head->text, &operation))
              status = timao_host_local(execution, operation, frame->values,
                                        frame->used, &result, error);
            else
              status = timao_error(error, "unbound_name", "unknown local call");
          }
          complete = true;
        }
      }
      break;
    }
    if (status == TIMAO_OK && complete && result == NULL) {
      timao_error(error, "invalid_argument", "evaluation returned no value");
      status = TIMAO_ERROR;
    }
    if (status != TIMAO_OK) {
      if (error->span.end == 0 && node != NULL)
        error->span = node->span;
      diagnose_stack(stack, error);
      if (catch_error(execution, &stack, error) != TIMAO_OK)
        break;
      status = TIMAO_OK;
    } else if (complete) {
      pop(execution, &stack, result);
      if (stack == NULL) {
        *out = result;
        break;
      }
    }
    if (++since_gc >= 64 && stack != NULL) {
      since_gc = 0;
      if (timao_heap_collect(&vm->heap, &vm->meter, error) != TIMAO_OK) {
        status = TIMAO_ERROR;
        break;
      }
    }
  }
  if (status != TIMAO_OK && error->source == NULL)
    diagnose_stack(stack, error);
  while (stack != NULL)
    pop(execution, &stack, NULL);
  return status;
}

enum timao_status timao_call(struct timao_execution *execution,
                             const struct timao_value *callable,
                             const struct timao_value *const *args,
                             size_t count, enum timao_context context,
                             const struct timao_value **out,
                             struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing call output");
  *out = NULL;
  if (execution == NULL || execution->vm == NULL || !execution->vm->active ||
      (count != 0 && args == NULL) || context < TIMAO_NORMAL ||
      context > TIMAO_PURE)
    return timao_error(error, "invalid_argument",
                       "invalid nested call context");
  struct timao_diagnostic fallback = {0};
  if (error == NULL)
    error = &fallback;
  const enum timao_context previous = execution->context;
  if (context == TIMAO_PURE)
    execution->context = TIMAO_PURE;
  struct frame *body =
      start_function(execution, NULL, callable, args, count, error);
  enum timao_status status = TIMAO_ERROR;
  if (body != NULL) {
    timao_heap_root_remove(&execution->vm->heap, &execution->vm->result_root);
    status =
        timao_heap_collect(&execution->vm->heap, &execution->vm->meter, error);
    if (status == TIMAO_OK)
      status = run(execution, body, out, error);
    else {
      timao_diagnostic_at(
          error, body->program->source,
          body->program->nodes[callable->as.function.definition].span);
      pop(execution, &body, NULL);
    }
    if (status == TIMAO_OK)
      publish_result(execution->vm, *out);
  }
  execution->context = previous;
  if (error == &fallback)
    timao_diagnostic_destroy(&fallback);
  return status;
}

enum timao_status timao_invoke_handler(struct timao_vm *vm,
                                       const struct timao_value *callable,
                                       const struct timao_value *event,
                                       const struct timao_value **out,
                                       struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing handler output");
  *out = NULL;
  if (vm == NULL || vm->active)
    return timao_error(error, "invalid_argument",
                       "reentrant handler invocation");
  timao_diagnostic_destroy(error);
  timao_meter_init(&vm->meter, vm->limits.steps, vm->host.context,
                   vm->host.cancelled);
  vm->active = true;
  struct timao_execution execution = {.vm = vm};
  const enum timao_status status =
      timao_call(&execution, callable, &event, 1, TIMAO_NORMAL, out, error);
  vm->active = false;
  return status;
}

enum timao_status timao_invoke_handler_json(
    struct timao_vm *vm, const struct timao_value *callable,
    const struct leme_public_value *event, const struct timao_value **out,
    struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing handler output");
  *out = NULL;
  if (vm == NULL || vm->active || event == NULL)
    return timao_error(error, "invalid_argument",
                       "invalid or reentrant handler invocation");
  if (callable == NULL || !timao_heap_owns(&vm->heap, callable->allocation) ||
      callable->kind != TIMAO_CALLABLE)
    return timao_error(error, "type_error", "local callable required");
  if (callable->as.function.arity != 1)
    return timao_error(error, "arity_error",
                       "handler requires one event argument");
  timao_diagnostic_destroy(error);
  timao_meter_init(&vm->meter, vm->limits.steps, vm->host.context,
                   vm->host.cancelled);
  vm->active = true;
  struct timao_heap_root root = {0};
  timao_heap_root_add(&vm->heap, &root, callable->allocation);
  timao_heap_root_remove(&vm->heap, &vm->result_root);
  struct timao_execution execution = {.vm = vm};
  const struct timao_value *argument = NULL;
  enum timao_status status = timao_heap_collect(&vm->heap, &vm->meter, error);
  if (status == TIMAO_OK)
    status = timao_import(&execution, event, &argument, error);
  if (status == TIMAO_OK)
    status = timao_call(&execution, callable, &argument, 1, TIMAO_NORMAL, out,
                        error);
  timao_heap_root_remove(&vm->heap, &root);
  vm->active = false;
  return status;
}

enum timao_status timao_eval_form(struct timao_vm *vm,
                                  struct timao_program *program, size_t index,
                                  const struct timao_value **out,
                                  struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing result output");
  *out = NULL;
  if (vm == NULL || program == NULL || index >= program->form_count ||
      vm->active)
    return timao_error(error, "invalid_argument",
                       "invalid or reentrant evaluation");
  if (program->source->input.mode == TIMAO_FILE) {
    if ((vm->file_program != NULL && vm->file_program != program) ||
        index != vm->file_next)
      return timao_error(error, "invalid_argument",
                         "file forms must execute once in source order");
    if (vm->file_program == NULL) {
      vm->file_program = program;
      timao_program_ref(program);
    }
    ++vm->file_next;
  }
  struct timao_diagnostic fallback = {0};
  if (error == NULL)
    error = &fallback;
  timao_diagnostic_destroy(error);
  timao_meter_init(&vm->meter, vm->limits.steps, vm->host.context,
                   vm->host.cancelled);
  if (vm->globals == NULL) {
    const struct timao_member binding = {.key = LEME_PUBLIC_TEXT("args"),
                                         .value = vm->args};
    struct timao_environment *environment = NULL;
    if (timao_environment_create(vm, NULL, &binding, 1, &environment, error) !=
        TIMAO_OK)
      return TIMAO_ERROR;
    publish_globals(vm, environment);
  }
  vm->active = true;
  struct timao_execution execution = {.vm = vm};
  struct frame *frame = push(&execution, NULL, program, program->forms[index],
                             vm->globals, NULL, error);
  enum timao_status status = TIMAO_ERROR;
  if (frame != NULL) {
    timao_heap_root_remove(&vm->heap, &vm->result_root);
    status = timao_heap_collect(&vm->heap, &vm->meter, error);
    if (status == TIMAO_OK)
      status = run(&execution, frame, out, error);
    else
      pop(&execution, &frame, NULL);
    if (status == TIMAO_OK)
      publish_result(vm, *out);
  }
  vm->active = false;
  if (error == &fallback)
    timao_diagnostic_destroy(&fallback);
  return status;
}
