#include "timao/repl-internal.h"
#include "timao/source.h"
#include "control/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static enum timao_status resource(struct timao_diagnostic *error) {
  return timao_error(error,
                     errno == ENOSPC || errno == EOVERFLOW ? "resource_limit"
                                                           : "out_of_memory",
                     "unable to retain REPL input");
}

enum timao_status timao_repl_editor_create(struct timao_runtime *runtime,
                                           int input_fd,
                                           struct timao_repl_editor **out,
                                           struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing editor output");
  *out = NULL;
  if (runtime == NULL)
    return timao_error(error, "invalid_argument", "missing editor runtime");
  const int flags = fcntl(input_fd, F_GETFL);
  if (flags < 0)
    return timao_error(error, "io_error", "invalid REPL input descriptor");
  struct timao_repl_editor *editor =
      leme_control_alloc(runtime->account, sizeof(*editor));
  if (editor == NULL)
    return resource(error);
  *editor = (struct timao_repl_editor){.runtime = runtime,
                                       .fd = input_fd,
                                       .flags = flags,
                                       .history_index = SIZE_MAX};
  if ((flags & O_NONBLOCK) == 0) {
    if (fcntl(input_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      timao_error(error, "io_error", "unable to make REPL input nonblocking");
      goto failure;
    }
    editor->changed_flags = true;
  }
  editor->terminal = isatty(input_fd) == 1 && isatty(runtime->error_fd) == 1;
  if (editor->terminal) {
    if (tcgetattr(input_fd, &editor->original) < 0) {
      timao_error(error, "io_error", "unable to save terminal mode");
      goto failure;
    }
    editor->locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    if (editor->locale == (locale_t)0) {
      timao_error(error, "unavailable", "UTF-8 terminal locale unavailable");
      goto failure;
    }
    const enum leme_public_status made = timao_output_writer_create(
        runtime->account, runtime->error_fd, &editor->writer);
    if (made != LEME_PUBLIC_OK) {
      timao_error(error,
                  made == LEME_PUBLIC_OOM     ? "out_of_memory"
                  : made == LEME_PUBLIC_LIMIT ? "resource_limit"
                                              : "io_error",
                  "unable to create terminal writer");
      goto failure;
    }
    struct termios raw = editor->original;
    raw.c_lflag &= (tcflag_t) ~(tcflag_t)(ICANON | ECHO | ECHONL | IEXTEN);
    raw.c_lflag |= ISIG;
    raw.c_iflag &=
        (tcflag_t) ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | IXOFF | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    raw.c_cc[VINTR] = 3;
    if (tcsetattr(input_fd, TCSANOW, &raw) < 0) {
      timao_error(error, "io_error", "unable to set terminal mode");
      goto failure;
    }
    editor->changed_termios = true;
    if (timao_repl_editor_redraw(editor, error) != TIMAO_OK)
      goto failure;
  }
  *out = editor;
  return TIMAO_OK;
failure:
  if (timao_repl_editor_destroy(editor) < 0)
    timao_error(error, "io_error", "unable to restore editor setup");
  return TIMAO_ERROR;
}

static enum timao_status reserve(struct timao_repl_editor *editor,
                                 size_t wanted,
                                 struct timao_diagnostic *error) {
  const size_t limit = editor->runtime->limits.source_bytes;
  if (wanted > limit)
    return timao_error(error, "resource_limit",
                       "REPL input exceeds source byte limit");
  if (wanted <= editor->capacity)
    return TIMAO_OK;
  size_t capacity =
      editor->capacity != 0 ? editor->capacity : (limit < 256 ? limit : 256);
  while (capacity < wanted)
    capacity = capacity > limit / 2 ? limit : capacity * 2;
  char *replacement =
      editor->bytes != NULL
          ? leme_control_realloc(editor->bytes, capacity)
          : leme_control_alloc(editor->runtime->language_account, capacity);
  if (replacement == NULL)
    return resource(error);
  editor->bytes = replacement;
  editor->capacity = capacity;
  return TIMAO_OK;
}

enum timao_status timao_repl_replace(struct timao_repl_editor *editor,
                                     struct leme_public_text bytes,
                                     struct timao_diagnostic *error) {
  if (reserve(editor, bytes.length, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (bytes.length != 0)
    memcpy(editor->bytes, bytes.data, bytes.length);
  editor->length = bytes.length;
  editor->cursor = bytes.length;
  editor->continuation = false;
  return TIMAO_OK;
}

static enum timao_status insert(struct timao_repl_editor *editor,
                                unsigned char byte,
                                struct timao_diagnostic *error) {
  if (editor->length == editor->runtime->limits.source_bytes)
    return timao_error(error, "resource_limit",
                       "REPL input exceeds source byte limit");
  if (reserve(editor, editor->length + 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  timao_repl_history_edit(editor);
  if (editor->cursor < editor->length)
    memmove(editor->bytes + editor->cursor + 1, editor->bytes + editor->cursor,
            editor->length - editor->cursor);
  editor->bytes[editor->cursor++] = (char)byte;
  ++editor->length;
  return TIMAO_OK;
}

static size_t following(const struct timao_repl_editor *editor) {
  if (editor->cursor == editor->length)
    return editor->cursor;
  size_t width = 0;
  if (!timao_utf8_width(
          (struct leme_public_text){editor->bytes, editor->length},
          editor->cursor, &width))
    width = 1;
  return editor->cursor + width;
}

static void erase(struct timao_repl_editor *editor, bool backwards) {
  const size_t begin =
      backwards ? timao_repl_previous(editor, editor->cursor) : editor->cursor;
  const size_t end = backwards ? editor->cursor : following(editor);
  if (begin == end)
    return;
  timao_repl_history_edit(editor);
  memmove(editor->bytes + begin, editor->bytes + end, editor->length - end);
  editor->length -= end - begin;
  editor->cursor = begin;
}

static enum timao_status escaped(struct timao_repl_editor *editor,
                                 struct timao_diagnostic *error) {
  const unsigned char *bytes = editor->escape;
  const size_t length = editor->escape_length;
  editor->escape_length = 0;
  if (length == 3 && bytes[1] == '[') {
    if (bytes[2] == 'A' || bytes[2] == 'B')
      return timao_repl_history_move(editor, bytes[2] == 'A', error);
    if (bytes[2] == 'C') {
      editor->cursor = following(editor);
      return TIMAO_OK;
    }
    if (bytes[2] == 'D') {
      editor->cursor = timao_repl_previous(editor, editor->cursor);
      return TIMAO_OK;
    }
    if (bytes[2] == 'H' || bytes[2] == 'F') {
      editor->cursor = bytes[2] == 'H' ? 0 : editor->length;
      return TIMAO_OK;
    }
  }
  if (length == 3 && bytes[1] == 'O' && (bytes[2] == 'H' || bytes[2] == 'F')) {
    editor->cursor = bytes[2] == 'H' ? 0 : editor->length;
    return TIMAO_OK;
  }
  if (length == 4 && bytes[1] == '[' && bytes[3] == '~') {
    if (bytes[2] == '3') {
      erase(editor, false);
      return TIMAO_OK;
    }
    if (bytes[2] == '1' || bytes[2] == '4') {
      editor->cursor = bytes[2] == '1' ? 0 : editor->length;
      return TIMAO_OK;
    }
  }
  for (size_t i = 0; i < length; ++i)
    if (insert(editor, bytes[i], error) != TIMAO_OK)
      return TIMAO_ERROR;
  return TIMAO_OK;
}

static enum timao_repl_progress prepared(struct timao_repl_editor *editor,
                                         struct leme_public_text *form,
                                         struct timao_diagnostic *error) {
  struct timao_diagnostic fallback = {0};
  struct timao_diagnostic *diagnostic = error != NULL ? error : &fallback;
  struct timao_program *program = NULL;
  const struct timao_input input = {.bytes = {editor->bytes, editor->length},
                                    .name = LEME_PUBLIC_TEXT("<repl>"),
                                    .mode = TIMAO_REPL,
                                    .cancel_context = editor->runtime,
                                    .cancelled = timao_runtime_cancelled};
  const enum timao_status status =
      timao_prepare(editor->runtime->language_account, &editor->runtime->limits,
                    &input, &program, diagnostic);
  timao_program_unref(program);
  enum timao_repl_progress result = TIMAO_REPL_FORM;
  if (status == TIMAO_INCOMPLETE && !editor->eof) {
    timao_diagnostic_destroy(diagnostic);
    editor->continuation = true;
    result = TIMAO_REPL_WAIT;
  } else if ((status == TIMAO_ERROR &&
              (strcmp(diagnostic->code, "out_of_memory") == 0 ||
               strcmp(diagnostic->code, "resource_limit") == 0 ||
               strcmp(diagnostic->code, "cancelled") == 0)) ||
             timao_repl_history_store(editor, diagnostic) != TIMAO_OK)
    result = TIMAO_REPL_ERROR;
  if (result == TIMAO_REPL_FORM) {
    editor->ready = true;
    *form = input.bytes;
  }
  if (error == NULL)
    timao_diagnostic_destroy(&fallback);
  return result;
}

static enum timao_repl_progress key(struct timao_repl_editor *editor,
                                    unsigned char byte,
                                    struct leme_public_text *form,
                                    struct timao_diagnostic *error) {
  if (editor->terminal && byte == 4) {
    editor->length = 0;
    editor->cursor = 0;
    editor->escape_length = 0;
    editor->eof = true;
    return timao_repl_editor_hide(editor, error) == TIMAO_OK ? TIMAO_REPL_END
                                                             : TIMAO_REPL_ERROR;
  }
  if (editor->terminal && editor->escape_length != 0) {
    editor->escape[editor->escape_length++] = byte;
    const bool prefix =
        editor->escape_length == 2 && (byte == '[' || byte == 'O');
    if (!prefix &&
        (editor->escape_length == 2 || (byte >= 0x40 && byte <= 0x7e) ||
         editor->escape_length == sizeof(editor->escape))) {
      if (escaped(editor, error) != TIMAO_OK)
        return TIMAO_REPL_ERROR;
    }
    return TIMAO_REPL_WAIT;
  }
  if (editor->terminal && byte == 27) {
    editor->escape[0] = byte;
    editor->escape_length = 1;
    return TIMAO_REPL_WAIT;
  }
  if (editor->terminal && (byte == 127 || byte == 8)) {
    erase(editor, true);
    return TIMAO_REPL_WAIT;
  }
  if (editor->terminal && (byte == 1 || byte == 5)) {
    editor->cursor = byte == 1 ? 0 : editor->length;
    return TIMAO_REPL_WAIT;
  }
  if (editor->terminal && (byte == 16 || byte == 14))
    return timao_repl_history_move(editor, byte == 16, error) == TIMAO_OK
               ? TIMAO_REPL_WAIT
               : TIMAO_REPL_ERROR;
  if (editor->terminal && byte == '\r')
    byte = '\n';
  if (byte == '\n') {
    if (timao_repl_editor_redraw(editor, error) != TIMAO_OK)
      return TIMAO_REPL_ERROR;
    editor->cursor = editor->length;
  }
  if (insert(editor, byte, error) != TIMAO_OK)
    return TIMAO_REPL_ERROR;
  if (byte != '\n')
    return TIMAO_REPL_WAIT;
  const enum timao_repl_progress result = prepared(editor, form, error);
  if (result == TIMAO_REPL_FORM && editor->terminal) {
    if (timao_repl_write(editor, LEME_PUBLIC_TEXT("\r\n"), error) != TIMAO_OK)
      return TIMAO_REPL_ERROR;
    editor->visible = false;
  }
  return result;
}

enum timao_repl_progress
timao_repl_editor_step(struct timao_repl_editor *editor,
                       struct leme_public_text *form,
                       struct timao_diagnostic *error) {
  if (form == NULL)
    return TIMAO_REPL_ERROR;
  *form = (struct leme_public_text){0};
  if (editor == NULL)
    return TIMAO_REPL_ERROR;
  if (editor->ready) {
    *form = (struct leme_public_text){editor->bytes, editor->length};
    return TIMAO_REPL_FORM;
  }
  if (timao_runtime_cancelled(editor->runtime)) {
    timao_error(error, "cancelled", "REPL input interrupted");
    return TIMAO_REPL_ERROR;
  }
  if (editor->eof)
    return editor->length == 0 ? TIMAO_REPL_END : prepared(editor, form, error);
  if (editor->pending_position == editor->pending_length) {
    const ssize_t count =
        read(editor->fd, editor->pending, sizeof(editor->pending));
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return TIMAO_REPL_WAIT;
      timao_error(error, "io_error", "unable to read REPL input");
      return TIMAO_REPL_ERROR;
    }
    if (count == 0) {
      editor->eof = true;
      if (editor->escape_length != 0 && escaped(editor, error) != TIMAO_OK)
        return TIMAO_REPL_ERROR;
      return editor->length == 0 ? TIMAO_REPL_END
                                 : prepared(editor, form, error);
    }
    editor->pending_position = 0;
    editor->pending_length = (size_t)count;
  }
  while (editor->pending_position < editor->pending_length) {
    const enum timao_repl_progress result =
        key(editor, editor->pending[editor->pending_position++], form, error);
    if (result != TIMAO_REPL_WAIT)
      return result;
  }
  return timao_repl_editor_redraw(editor, error) == TIMAO_OK ? TIMAO_REPL_WAIT
                                                             : TIMAO_REPL_ERROR;
}

void timao_repl_editor_consume(struct timao_repl_editor *editor) {
  if (editor != NULL && editor->ready) {
    editor->ready = false;
    editor->length = 0;
    editor->cursor = 0;
    editor->continuation = false;
  }
}
enum timao_status timao_repl_editor_interrupt(struct timao_repl_editor *editor,
                                              struct timao_diagnostic *error) {
  if (editor == NULL)
    return timao_error(error, "invalid_argument", "missing editor");
  if (editor->terminal && tcflush(editor->fd, TCIFLUSH) < 0)
    return timao_error(error, "io_error",
                       "unable to discard interrupted terminal input");
  if (timao_repl_editor_hide(editor, error) != TIMAO_OK)
    return TIMAO_ERROR;
  editor->length = 0;
  editor->cursor = 0;
  editor->ready = false;
  editor->continuation = false;
  editor->pending_length = 0;
  editor->pending_position = 0;
  editor->escape_length = 0;
  timao_repl_history_edit(editor);
  if (timao_repl_write(editor, LEME_PUBLIC_TEXT("^C\r\n"), error) != TIMAO_OK)
    return TIMAO_ERROR;
  return timao_repl_editor_redraw(editor, error);
}
int timao_repl_editor_destroy(struct timao_repl_editor *editor) {
  if (editor == NULL)
    return 0;
  int failure = 0;
  if (timao_output_writer_destroy(editor->writer) < 0)
    failure = errno;
  if (editor->changed_termios &&
      tcsetattr(editor->fd, TCSANOW, &editor->original) < 0 && failure == 0)
    failure = errno;
  if (editor->changed_flags) {
    const int current = fcntl(editor->fd, F_GETFL);
    if ((current < 0 ||
         fcntl(editor->fd, F_SETFL, current & ~O_NONBLOCK) < 0) &&
        failure == 0)
      failure = errno;
  }
  if (editor->locale != (locale_t)0)
    freelocale(editor->locale);
  timao_repl_history_destroy(editor);
  leme_control_free(editor->bytes);
  leme_control_free(editor);
  if (failure == 0)
    return 0;
  errno = failure;
  return -1;
}
