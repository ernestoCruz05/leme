#include "timao/repl-internal.h"
#include "control/memory.h"
#include <errno.h>
#include <string.h>

void timao_repl_history_edit(struct timao_repl_editor *editor) {
  leme_control_free(editor->draft);
  editor->draft = NULL;
  editor->draft_length = 0;
  editor->history_index = SIZE_MAX;
}

static enum timao_status memory_error(struct timao_diagnostic *error) {
  return timao_error(error,
                     errno == ENOSPC || errno == EOVERFLOW ? "resource_limit"
                                                           : "out_of_memory",
                     "unable to retain editor history");
}

enum timao_status timao_repl_history_store(struct timao_repl_editor *editor,
                                           struct timao_diagnostic *error) {
  timao_repl_history_edit(editor);
  size_t length = editor->length;
  if (length != 0 && editor->bytes[length - 1] == '\n')
    --length;
  if (!editor->terminal || length == 0)
    return TIMAO_OK;
  const size_t overhead = leme_control_allocation_overhead();
  if (overhead >= 262144 || length > 262144 - overhead)
    return TIMAO_OK;
  const size_t cost = length + overhead;
  if (editor->history_count != 0) {
    const struct timao_repl_history *last =
        &editor->history[editor->history_count - 1];
    if (last->length == length &&
        memcmp(last->bytes, editor->bytes, length) == 0)
      return TIMAO_OK;
  }
  while (
      editor->history_count != 0 &&
      (editor->history_count == 64 || cost > 262144 - editor->history_bytes)) {
    editor->history_bytes -=
        leme_control_allocation_bytes(editor->history[0].bytes);
    leme_control_free(editor->history[0].bytes);
    --editor->history_count;
    memmove(editor->history, editor->history + 1,
            editor->history_count * sizeof(editor->history[0]));
    editor->history[editor->history_count] = (struct timao_repl_history){0};
  }
  char *copy = leme_control_alloc(editor->runtime->language_account, length);
  if (copy == NULL)
    return memory_error(error);
  memcpy(copy, editor->bytes, length);
  editor->history[editor->history_count++] =
      (struct timao_repl_history){.bytes = copy, .length = length};
  editor->history_bytes += leme_control_allocation_bytes(copy);
  return TIMAO_OK;
}

enum timao_status timao_repl_history_move(struct timao_repl_editor *editor,
                                          bool previous,
                                          struct timao_diagnostic *error) {
  if (editor->history_count == 0 ||
      (!previous && editor->history_index == SIZE_MAX))
    return TIMAO_OK;
  if (editor->history_index == SIZE_MAX) {
    char *draft = NULL;
    if (editor->length != 0) {
      draft =
          leme_control_alloc(editor->runtime->language_account, editor->length);
      if (draft == NULL)
        return memory_error(error);
      memcpy(draft, editor->bytes, editor->length);
    }
    editor->draft = draft;
    editor->draft_length = editor->length;
    editor->history_index = editor->history_count;
  }
  size_t index = editor->history_index;
  if (previous && index != 0)
    --index;
  else if (!previous && index < editor->history_count)
    ++index;
  const struct leme_public_text text =
      index == editor->history_count
          ? (struct leme_public_text){editor->draft, editor->draft_length}
          : (struct leme_public_text){editor->history[index].bytes,
                                      editor->history[index].length};
  const enum timao_status status = timao_repl_replace(editor, text, error);
  if (status != TIMAO_OK)
    return status;
  editor->history_index = index;
  if (index == editor->history_count)
    timao_repl_history_edit(editor);
  return TIMAO_OK;
}

void timao_repl_history_destroy(struct timao_repl_editor *editor) {
  timao_repl_history_edit(editor);
  for (size_t i = 0; i < editor->history_count; ++i)
    leme_control_free(editor->history[i].bytes);
  editor->history_count = 0;
  editor->history_bytes = 0;
}
