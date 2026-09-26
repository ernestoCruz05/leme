#include "timao/parser.h"
#include "timao/diagnostic.h"
#include <stdlib.h>
#include <string.h>

const struct timao_node *timao_node_at(const struct timao_program *program,
                                       uint32_t index) {
  return index == 0 || index >= program->count ? NULL : &program->nodes[index];
}
uint32_t timao_child(const struct timao_program *program, uint32_t index,
                     size_t ordinal) {
  if (program == NULL || !timao_index_valid(index, program->count))
    return 0;
  const struct timao_node *node = timao_node_at(program, index);
  if (node == NULL || !timao_index_valid(ordinal, node->count))
    return 0;
  uint32_t child = node->first;
  while (child != 0 && ordinal != 0) {
    child = program->nodes[child].next;
    --ordinal;
  }
  return child;
}
bool timao_node_is(const struct timao_node *node, const char *text) {
  const size_t length = strlen(text);
  return node != NULL && node->kind == TIMAO_TOKEN_NAME &&
         node->text.length == length &&
         memcmp(node->text.data, text, length) == 0;
}
void timao_program_ref(struct timao_program *program) {
  if (program == NULL)
    return;
  if (program->references == SIZE_MAX)
    abort();
  ++program->references;
}
void timao_program_unref(struct timao_program *program) {
  if (program == NULL || --program->references != 0)
    return;
  if (program->nodes != NULL)
    for (size_t i = 1; i < program->count; ++i)
      timao_memory_free(program->nodes[i].owned);
  timao_memory_free(program->nodes);
  timao_memory_free(program->forms);
  timao_source_unref(program->source);
  timao_memory_free(program);
}
size_t timao_program_forms(const struct timao_program *program) {
  return program == NULL ? 0 : program->form_count;
}
static enum timao_status append(struct timao_program *program,
                                struct timao_token *token, uint32_t *out,
                                struct timao_diagnostic *error) {
  if (program->count - 1 >= program->limits.nodes)
    return timao_error(error, "resource_limit", "parsed node limit exceeded");
  if (program->count >= program->capacity) {
    size_t capacity = program->capacity == 0 ? 32 : program->capacity * 2;
    if (capacity > program->limits.nodes + 1)
      capacity = program->limits.nodes + 1;
    struct timao_node *nodes = timao_memory_alloc(
        program->source->account, capacity * sizeof(*nodes), error);
    if (nodes == NULL)
      return TIMAO_ERROR;
    if (program->nodes != NULL) {
      for (size_t offset = 0; offset < program->count;) {
        if (timao_source_check(program->source, error) != TIMAO_OK) {
          timao_memory_free(nodes);
          return TIMAO_ERROR;
        }
        const size_t count =
            program->count - offset > 128 ? 128 : program->count - offset;
        memcpy(nodes + offset, program->nodes + offset, count * sizeof(*nodes));
        offset += count;
      }
    }
    timao_memory_free(program->nodes);
    program->nodes = nodes;
    program->capacity = capacity;
  }
  *out = (uint32_t)program->count++;
  program->nodes[*out] = (struct timao_node){.kind = token->kind,
                                             .span = token->span,
                                             .text = token->text,
                                             .number = token->number,
                                             .owned = token->owned};
  token->owned = NULL;
  return TIMAO_OK;
}

enum timao_status timao_prepare(struct leme_public_budget *account,
                                const struct timao_limits *limits,
                                const struct timao_input *input,
                                struct timao_program **out,
                                struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing program output");
  *out = NULL;
  if (input == NULL || input->mode < TIMAO_FILE || input->mode > TIMAO_REPL)
    return timao_error(error, "invalid_argument", "invalid source mode");
  struct timao_limits selected = {0};
  if (timao_limits_resolve(limits, &selected, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct leme_public_budget *child = NULL;
  const enum leme_public_status created =
      leme_public_budget_child(account, selected.memory_bytes, &child);
  if (created != LEME_PUBLIC_OK)
    return timao_error(
        error, created == LEME_PUBLIC_OOM ? "out_of_memory" : "resource_limit",
        "program account unavailable");
  struct timao_program *program =
      timao_memory_alloc(child, sizeof(*program), error);
  if (program == NULL) {
    leme_public_budget_unref(child);
    return TIMAO_ERROR;
  }
  program->references = 1;
  program->count = 1;
  program->limits = selected;
  enum timao_status status =
      timao_source_create(child, &selected, input, &program->source, error);
  leme_public_budget_unref(child);
  if (status != TIMAO_OK)
    goto fail;
  struct timao_lexer lexer = {.source = program->source};
  struct {
    uint32_t node;
    uint32_t last;
  } stack[128] = {0};
  size_t depth = 0;
  uint32_t first = 0, last = 0;
  for (;;) {
    struct timao_token token = {0};
    status = timao_lexer_next(&lexer, &token, error);
    if (status != TIMAO_OK) {
      timao_token_finish(&token);
      goto fail;
    }
    if (token.kind == TIMAO_TOKEN_END) {
      if (depth != 0) {
        status =
            input->mode == TIMAO_REPL
                ? TIMAO_INCOMPLETE
                : timao_error(error, "syntax_error", "unterminated expression");
        if (error != NULL)
          error->span = program->nodes[stack[depth - 1].node].span;
        goto fail;
      }
      break;
    }
    if (token.kind == TIMAO_TOKEN_CLOSE) {
      if (depth == 0) {
        status =
            timao_error(error, "syntax_error", "unmatched closing parenthesis");
        if (error != NULL)
          error->span = token.span;
        goto fail;
      }
      program->nodes[stack[--depth].node].span.end = token.span.end;
      continue;
    }
    if (token.kind == TIMAO_TOKEN_OPEN && depth >= selected.nesting) {
      status =
          timao_error(error, "resource_limit", "syntax nesting limit exceeded");
      goto fail;
    }
    uint32_t index = 0;
    status = append(program, &token, &index, error);
    timao_token_finish(&token);
    if (status != TIMAO_OK)
      goto fail;
    if (depth == 0) {
      if (last != 0)
        program->nodes[last].next = index;
      else
        first = index;
      last = index;
      ++program->form_count;
    } else {
      const uint32_t parent = stack[depth - 1].node;
      if (stack[depth - 1].last != 0)
        program->nodes[stack[depth - 1].last].next = index;
      else
        program->nodes[parent].first = index;
      ++program->nodes[parent].count;
      stack[depth - 1].last = index;
    }
    if (program->nodes[index].kind == TIMAO_TOKEN_OPEN) {
      stack[depth].node = index;
      stack[depth].last = 0;
      ++depth;
    }
  }
  if (input->mode == TIMAO_EXPRESSION && program->form_count != 1) {
    status = timao_error(error, "syntax_error",
                         "expression mode requires exactly one form");
    goto fail;
  }
  if (program->form_count != 0) {
    program->forms = timao_memory_alloc(
        program->source->account, program->form_count * sizeof(*program->forms),
        error);
    if (program->forms == NULL) {
      status = TIMAO_ERROR;
      goto fail;
    }
    for (size_t i = 0; i < program->form_count; ++i) {
      if (i % 128 == 0 &&
          timao_source_check(program->source, error) != TIMAO_OK) {
        status = TIMAO_ERROR;
        goto fail;
      }
      program->forms[i] = first;
      first = program->nodes[first].next;
    }
  }
  status = timao_validate(program, error);
  if (status != TIMAO_OK)
    goto fail;
  program->source->input.cancelled = NULL;
  program->source->input.cancel_context = NULL;
  *out = program;
  return TIMAO_OK;
fail:
  if (program->source != NULL) {
    program->source->input.cancelled = NULL;
    program->source->input.cancel_context = NULL;
    if (status == TIMAO_ERROR && error != NULL)
      timao_diagnostic_at(error, program->source, error->span);
  } else if (status == TIMAO_ERROR)
    timao_diagnostic_input(error, input);
  timao_program_unref(program);
  return status;
}
