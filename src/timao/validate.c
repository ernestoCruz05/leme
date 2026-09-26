#include "timao/parser.h"
#include "timao/diagnostic.h"
#include "control/registry.h"
#include <string.h>

struct validation {
  struct timao_program *program;
  struct timao_diagnostic *error;
  size_t remaining;
};

static enum timao_status invalid(struct validation *v, uint32_t index,
                                 const char *code, const char *message) {
  timao_error(v->error, code, message);
  if (v->error != NULL)
    v->error->span = v->program->nodes[index].span;
  return TIMAO_ERROR;
}
static bool tick(struct validation *v) {
  if (v->remaining == 0) {
    timao_error(v->error, "resource_limit", "validation work exhausted");
    return false;
  }
  --v->remaining;
  return timao_source_check(v->program->source, v->error) == TIMAO_OK;
}
static bool named(const struct timao_node *node, const char *name) {
  return timao_node_is(node, name);
}
static enum timao_status same_name(struct validation *v,
                                   const struct timao_node *left,
                                   const struct timao_node *right, bool *out) {
  *out = false;
  if (left->text.length != right->text.length)
    return TIMAO_OK;
  for (size_t offset = 0; offset < left->text.length;) {
    if (!tick(v))
      return TIMAO_ERROR;
    const size_t count =
        left->text.length - offset > 128 ? 128 : left->text.length - offset;
    if (memcmp(left->text.data + offset, right->text.data + offset, count) != 0)
      return TIMAO_OK;
    offset += count;
  }
  *out = true;
  return TIMAO_OK;
}
static bool binding_name(const struct timao_node *node, bool definition) {
  if (node == NULL || node->kind != TIMAO_TOKEN_NAME)
    return false;
  if (leme_control_operator_find(node->text) != NULL)
    return false;
  const char *const reserved[] = {
      "def", "defn",  "let",  "do",     "for-each", "try",    "catch", "query",
      "act", "watch", "emit", "launch", "on",       "cancel", "await", "->"};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
    if (named(node, reserved[i]))
      return false;
  return !definition || !named(node, "args");
}
static enum timao_status expand(struct validation *v, uint32_t index,
                                size_t depth) {
  if (!tick(v))
    return TIMAO_ERROR;
  if (!timao_index_valid(index, v->program->count))
    return timao_error(v->error, "invalid_argument", "invalid syntax node");
  if (!timao_index_valid(depth, v->program->limits.nesting + 1))
    return invalid(v, index, "resource_limit", "expanded nesting exceeded");
  struct timao_node *node = &v->program->nodes[index];
  if (node->kind != TIMAO_TOKEN_OPEN)
    return TIMAO_OK;
  for (uint32_t child = node->first; child != 0;
       child = v->program->nodes[child].next)
    if (expand(v, child, depth + 1) != TIMAO_OK)
      return TIMAO_ERROR;
  if (named(timao_node_at(v->program, node->first), "->"))
    return timao_pipeline(v->program, index, v->error);
  return TIMAO_OK;
}

static enum timao_status expression(struct validation *v, uint32_t index,
                                    size_t depth, bool top, bool item,
                                    int remote);
static enum timao_status sequence(struct validation *v, uint32_t first,
                                  size_t depth, bool item, int remote) {
  for (uint32_t index = first; index != 0;
       index = v->program->nodes[index].next)
    if (expression(v, index, depth, false, item, remote) != TIMAO_OK)
      return TIMAO_ERROR;
  return TIMAO_OK;
}
static enum timao_status unique_bindings(struct validation *v, uint32_t list,
                                         bool pairs) {
  const struct timao_node *container = &v->program->nodes[list];
  if (container->kind != TIMAO_TOKEN_OPEN)
    return invalid(v, list, "syntax_error", "binding list required");
  for (uint32_t a = container->first; a != 0; a = v->program->nodes[a].next) {
    if (!tick(v))
      return TIMAO_ERROR;
    const struct timao_node *entry = &v->program->nodes[a];
    const uint32_t name = pairs ? entry->first : a;
    if ((pairs && (entry->kind != TIMAO_TOKEN_OPEN || entry->count != 2)) ||
        !binding_name(timao_node_at(v->program, name), false))
      return invalid(v, a, "syntax_error", "invalid binding or parameter");
    for (uint32_t b = container->first; b != a; b = v->program->nodes[b].next) {
      if (!tick(v))
        return TIMAO_ERROR;
      const uint32_t other = pairs ? v->program->nodes[b].first : b;
      bool matches = false;
      if (same_name(v, &v->program->nodes[name], &v->program->nodes[other],
                    &matches) != TIMAO_OK)
        return TIMAO_ERROR;
      if (matches)
        return invalid(v, a, "syntax_error", "duplicate binding or parameter");
    }
  }
  return TIMAO_OK;
}

static enum timao_status expression(struct validation *v, uint32_t index,
                                    size_t depth, bool top, bool item,
                                    int remote) {
  if (!tick(v))
    return TIMAO_ERROR;
  if (!timao_index_valid(index, v->program->count))
    return timao_error(v->error, "invalid_argument", "invalid syntax node");
  const struct timao_node *node = &v->program->nodes[index];
  if (node->kind != TIMAO_TOKEN_OPEN) {
    if (node->kind == TIMAO_TOKEN_FIELD && !item)
      return invalid(v, index, "syntax_error",
                     "field selector outside item scope");
    return TIMAO_OK;
  }
  if (!timao_index_valid(depth, v->program->limits.nesting))
    return invalid(v, index, "resource_limit", "expanded nesting exceeded");
  const struct timao_node *head = timao_node_at(v->program, node->first);
  if (head == NULL || head->kind != TIMAO_TOKEN_NAME)
    return invalid(v, index, "syntax_error", "nonempty named call required");
  const size_t count = node->count - 1;
  const uint32_t first = head->next;
  const struct leme_control_operator *op =
      leme_control_operator_find(head->text);
  if (remote != 0) {
    if (op == NULL ||
        (op->effect == LEME_CONTROL_EFFECT_ACTION) != (remote == 2))
      return invalid(v, index, "invalid_remote_expression",
                     "call is not allowed at this remote boundary");
    if (count < op->min_args || count > op->max_args)
      return invalid(v, index, "arity_error", "wrong public operator arity");
    size_t ordinal = 0;
    for (uint32_t arg = first; arg != 0;
         arg = v->program->nodes[arg].next, ++ordinal) {
      const bool scoped = leme_control_operator_arg_is_item_scope(op, ordinal);
      if (expression(v, arg, depth + 1, false, item || scoped, 1) != TIMAO_OK)
        return TIMAO_ERROR;
    }
    if (op->opcode == LEME_CONTROL_OP_ITEM && !item)
      return invalid(v, index, "syntax_error", "item outside collection scope");
    return TIMAO_OK;
  }
  if (named(head, "query") || named(head, "act") || named(head, "watch")) {
    if (count != 1)
      return invalid(v, index, "arity_error",
                     "remote boundary needs one expression");
    if (named(head, "act") && v->program->nodes[first].kind != TIMAO_TOKEN_OPEN)
      return invalid(v, index, "invalid_remote_expression",
                     "action call required");
    return expression(v, first, depth + 1, false, false,
                      named(head, "act") ? 2 : 1);
  }
  if (named(head, "def") || named(head, "defn")) {
    const bool function = named(head, "defn");
    if (!top || (function ? count < 3 : count != 2) ||
        !binding_name(timao_node_at(v->program, first), true))
      return invalid(v, index, "syntax_error", "invalid top-level definition");
    const uint32_t second = v->program->nodes[first].next;
    if (!function)
      return expression(v, second, depth + 1, false, item, 0);
    if (unique_bindings(v, second, false) != TIMAO_OK)
      return TIMAO_ERROR;
    return sequence(v, v->program->nodes[second].next, depth + 1, false, 0);
  }
  if (named(head, "let")) {
    if (count < 2)
      return invalid(v, index, "syntax_error", "let needs bindings and body");
    if (unique_bindings(v, first, true) != TIMAO_OK)
      return TIMAO_ERROR;
    for (uint32_t entry = v->program->nodes[first].first; entry != 0;
         entry = v->program->nodes[entry].next)
      if (expression(v, timao_child(v->program, entry, 1), depth + 1, false,
                     item, 0) != TIMAO_OK)
        return TIMAO_ERROR;
    return sequence(v, v->program->nodes[first].next, depth + 1, item, 0);
  }
  if (named(head, "for-each")) {
    if (count < 3 || !binding_name(timao_node_at(v->program, first), false))
      return invalid(v, index, "syntax_error",
                     "for-each needs a binding, collection and body");
    return sequence(v, v->program->nodes[first].next, depth + 1, item, 0);
  }
  if (named(head, "try")) {
    const uint32_t handler = first == 0 ? 0 : v->program->nodes[first].next;
    const struct timao_node *catcher = timao_node_at(v->program, handler);
    if (count != 2 || catcher == NULL || catcher->kind != TIMAO_TOKEN_OPEN ||
        catcher->count < 3 ||
        !named(timao_node_at(v->program, catcher->first), "catch") ||
        !binding_name(
            timao_node_at(v->program, timao_child(v->program, handler, 1)),
            false))
      return invalid(v, index, "syntax_error",
                     "try requires a catch binding and body");
    if (expression(v, first, depth + 1, false, item, 0) != TIMAO_OK)
      return TIMAO_ERROR;
    return sequence(v, timao_child(v->program, handler, 2), depth + 1, item, 0);
  }
  if (named(head, "catch"))
    return invalid(v, index, "syntax_error", "catch outside try");
  if (named(head, "do"))
    return sequence(v, first, depth + 1, item, 0);
  const char *const hosts[] = {"emit", "launch", "on", "cancel", "await"};
  const size_t arities[] = {1, 1, 2, 1, 3};
  for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); ++i)
    if (named(head, hosts[i]) && count != arities[i])
      return invalid(v, index, "arity_error", "wrong host form arity");
  if (op != NULL) {
    if (count < op->min_args || count > op->max_args)
      return invalid(v, index, "arity_error", "wrong operator arity");
    if (op->opcode == LEME_CONTROL_OP_ITEM && !item)
      return invalid(v, index, "syntax_error", "item outside collection scope");
    if (op->opcode == LEME_CONTROL_OP_OBJECT && count % 2 != 0)
      return invalid(v, index, "arity_error", "object needs key/value pairs");
  }
  size_t ordinal = 0;
  for (uint32_t arg = first; arg != 0;
       arg = v->program->nodes[arg].next, ++ordinal) {
    const bool scoped =
        op != NULL && leme_control_operator_arg_is_item_scope(op, ordinal);
    if (expression(v, arg, depth + 1, false, item || scoped, 0) != TIMAO_OK)
      return TIMAO_ERROR;
  }
  return TIMAO_OK;
}

enum timao_status timao_validate(struct timao_program *program,
                                 struct timao_diagnostic *error) {
  struct validation v = {
      .program = program, .error = error, .remaining = 1000000};
  for (size_t i = 0; i < program->form_count; ++i) {
    if (expand(&v, program->forms[i], 0) != TIMAO_OK)
      return TIMAO_ERROR;
    if (expression(&v, program->forms[i], 0, true, false, 0) != TIMAO_OK)
      return TIMAO_ERROR;
    const struct timao_node *form = &program->nodes[program->forms[i]];
    const struct timao_node *head = timao_node_at(program, form->first);
    if (program->source->input.mode != TIMAO_FILE ||
        (!named(head, "def") && !named(head, "defn")))
      continue;
    const struct timao_node *name = timao_node_at(program, head->next);
    for (size_t j = 0; j < i; ++j) {
      if (!tick(&v))
        return TIMAO_ERROR;
      const struct timao_node *old_head =
          timao_node_at(program, program->nodes[program->forms[j]].first);
      if (named(old_head, "def") || named(old_head, "defn")) {
        bool matches = false;
        if (same_name(&v, name, timao_node_at(program, old_head->next),
                      &matches) != TIMAO_OK)
          return TIMAO_ERROR;
        if (matches)
          return invalid(&v, program->forms[i], "duplicate_definition",
                         "duplicate file definition");
      }
    }
  }
  return TIMAO_OK;
}
