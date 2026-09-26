#ifndef TIMAO_REPL_INTERNAL_H
#define TIMAO_REPL_INTERNAL_H
#include "timao/repl.h"
#include "timao/runtime-internal.h"
#include <locale.h>
#include <termios.h>

struct timao_repl_history {
  char *bytes;
  size_t length;
};
struct timao_repl_editor {
  struct timao_runtime *runtime;
  char *bytes;
  size_t length, capacity, cursor;
  unsigned char pending[256], escape[16];
  size_t pending_length, pending_position, escape_length;
  int fd, flags;
  bool changed_flags, eof, ready, terminal, visible, continuation,
      changed_termios;
  struct termios original;
  locale_t locale;
  struct timao_output_writer *writer;
  struct timao_repl_history history[64];
  size_t history_count, history_bytes, history_index;
  char *draft;
  size_t draft_length;
};
size_t timao_repl_previous(const struct timao_repl_editor *editor,
                           size_t cursor);
enum timao_status timao_repl_write(struct timao_repl_editor *editor,
                                   struct leme_public_text bytes,
                                   struct timao_diagnostic *error);
enum timao_status timao_repl_replace(struct timao_repl_editor *editor,
                                     struct leme_public_text bytes,
                                     struct timao_diagnostic *error);
enum timao_status timao_repl_history_store(struct timao_repl_editor *editor,
                                           struct timao_diagnostic *error);
enum timao_status timao_repl_history_move(struct timao_repl_editor *editor,
                                          bool previous,
                                          struct timao_diagnostic *error);
void timao_repl_history_edit(struct timao_repl_editor *editor);
void timao_repl_history_destroy(struct timao_repl_editor *editor);
#endif
