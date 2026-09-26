#ifndef TIMAO_REPL_H
#define TIMAO_REPL_H
#include "timao/runtime.h"

struct timao_repl_editor;
enum timao_repl_progress {
  TIMAO_REPL_WAIT,
  TIMAO_REPL_FORM,
  TIMAO_REPL_END,
  TIMAO_REPL_ERROR
};
enum timao_status timao_repl_editor_create(struct timao_runtime *runtime,
                                           int input_fd,
                                           struct timao_repl_editor **out,
                                           struct timao_diagnostic *error);
enum timao_repl_progress
timao_repl_editor_step(struct timao_repl_editor *editor,
                       struct leme_public_text *form,
                       struct timao_diagnostic *error);
void timao_repl_editor_consume(struct timao_repl_editor *editor);
enum timao_status timao_repl_editor_interrupt(struct timao_repl_editor *editor,
                                              struct timao_diagnostic *error);
enum timao_status timao_repl_editor_hide(struct timao_repl_editor *editor,
                                         struct timao_diagnostic *error);
enum timao_status timao_repl_editor_redraw(struct timao_repl_editor *editor,
                                           struct timao_diagnostic *error);
int timao_repl_editor_destroy(struct timao_repl_editor *editor);
#endif
