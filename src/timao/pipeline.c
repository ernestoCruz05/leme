#include "timao/parser.h"
#include "timao/diagnostic.h"

enum timao_status timao_pipeline(struct timao_program *program, uint32_t index,
                                 struct timao_diagnostic *error) {
  struct timao_node *node = &program->nodes[index];
  if (node->count < 2)
    return timao_error(error, "arity_error", "pipeline requires a source");
  const uint32_t head = node->first;
  uint32_t value = program->nodes[head].next;
  uint32_t stage = program->nodes[value].next;
  const uint32_t following = node->next;
  program->nodes[value].next = 0;
  while (stage != 0) {
    if (timao_source_check(program->source, error) != TIMAO_OK)
      return TIMAO_ERROR;
    struct timao_node *call = &program->nodes[stage];
    if (call->kind != TIMAO_TOKEN_OPEN || call->count == 0 ||
        program->nodes[call->first].kind != TIMAO_TOKEN_NAME)
      return timao_error(error, "syntax_error",
                         "pipeline stage must be a nonempty call");
    const uint32_t next = call->next;
    const uint32_t args = program->nodes[call->first].next;
    program->nodes[call->first].next = value;
    program->nodes[value].next = args;
    ++call->count;
    call->next = 0;
    value = stage;
    stage = next;
  }
  *node = program->nodes[value];
  program->nodes[value].owned = NULL;
  node->next = following;
  return TIMAO_OK;
}
