#include "timao/repl-internal.h"
#include "timao/source.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>

size_t timao_repl_previous(const struct timao_repl_editor *editor,
                           size_t cursor) {
  if (cursor == 0)
    return 0;
  size_t start = cursor - 1;
  while (start != 0 && cursor - start < 4 &&
         ((unsigned char)editor->bytes[start] & 0xc0) == 0x80)
    --start;
  size_t width = 0;
  const struct leme_public_text text = {editor->bytes, editor->length};
  return timao_utf8_width(text, start, &width) && width == cursor - start
             ? start
             : cursor - 1;
}

enum timao_status timao_repl_write(struct timao_repl_editor *editor,
                                   struct leme_public_text bytes,
                                   struct timao_diagnostic *error) {
  if (!editor->terminal)
    return TIMAO_OK;
  const struct timao_output_buffer buffer = {.data = bytes.data,
                                             .length = bytes.length};
  size_t offset = 0;
  for (;;) {
    const enum timao_output_progress progress =
        timao_output_write(editor->writer, &buffer, &offset,
                           timao_runtime_cancelled(editor->runtime));
    if (progress == TIMAO_OUTPUT_DONE)
      return TIMAO_OK;
    if (progress == TIMAO_OUTPUT_ERROR) {
      editor->runtime->io_error = true;
      return timao_error(error, "io_error", "terminal output failed");
    }
    if (progress == TIMAO_OUTPUT_CANCELLED)
      return timao_error(error, "cancelled", "terminal output interrupted");
    const enum timao_runtime_wait waited = timao_runtime_poll_fd(
        editor->runtime, UINT64_MAX,
        &(const struct timao_runtime_poll_input){
            .fd = editor->runtime->error_fd, .events = POLLOUT});
    if (waited != TIMAO_RUNTIME_PROGRESS)
      return timao_error(
          error, waited == TIMAO_RUNTIME_CANCELLED ? "cancelled" : "io_error",
          "terminal output wait stopped");
  }
}

struct glyph {
  char bytes[12];
  size_t length, cells, consumed;
};
static struct glyph glyph(const struct timao_repl_editor *editor, size_t at) {
  const struct leme_public_text text = {editor->bytes, editor->length};
  struct glyph out = {.consumed = 1};
  size_t width = 0;
  const unsigned char first = (unsigned char)text.data[at];
  uint32_t scalar = first;
  bool valid = timao_utf8_width(text, at, &width);
  if (valid) {
    out.consumed = width;
    if (width > 1) {
      scalar = first & (width == 2 ? 0x1fu : width == 3 ? 0x0fu : 0x07u);
      for (size_t i = 1; i < width; ++i)
        scalar = (scalar << 6) | ((unsigned char)text.data[at + i] & 0x3fu);
    }
  }
  int cells = -1;
  if (valid && scalar <= (uint32_t)WCHAR_MAX)
    cells = wcwidth((wchar_t)scalar);
  if (first == '\n' || first == '\r' || first == '\t') {
    out.bytes[0] = '\\';
    out.bytes[1] = (char)(first == '\n' ? 'n' : first == '\r' ? 'r' : 't');
    out.length = 2;
    out.cells = 2;
  } else if (!valid || cells < 0 || scalar < 32 || scalar == 127 ||
             (scalar >= 128 && scalar <= 159)) {
    const int length =
        !valid || scalar < 128
            ? snprintf(out.bytes, sizeof(out.bytes), "\\x%02x", (unsigned)first)
        : scalar <= 65535 ? snprintf(out.bytes, sizeof(out.bytes), "\\u%04x",
                                     (unsigned)scalar)
                          : snprintf(out.bytes, sizeof(out.bytes), "\\U%08x",
                                     (unsigned)scalar);
    if (length > 0 && (size_t)length < sizeof(out.bytes))
      out.length = (size_t)length;
    else {
      out.bytes[0] = '?';
      out.length = 1;
    }
    out.cells = out.length;
  } else {
    memcpy(out.bytes, text.data + at, width);
    out.length = width;
    out.cells = (size_t)cells;
  }
  return out;
}

enum timao_status timao_repl_editor_hide(struct timao_repl_editor *editor,
                                         struct timao_diagnostic *error) {
  if (editor == NULL)
    return timao_error(error, "invalid_argument", "missing editor");
  if (!editor->visible)
    return TIMAO_OK;
  const enum timao_status status =
      timao_repl_write(editor, LEME_PUBLIC_TEXT("\r\x1b[2K"), error);
  if (status == TIMAO_OK)
    editor->visible = false;
  return status;
}

enum timao_status timao_repl_editor_redraw(struct timao_repl_editor *editor,
                                           struct timao_diagnostic *error) {
  if (editor == NULL)
    return timao_error(error, "invalid_argument", "missing editor");
  if (!editor->terminal || editor->ready || editor->eof)
    return TIMAO_OK;
  struct winsize size = {0};
  size_t columns = 80;
  if (ioctl(editor->fd, TIOCGWINSZ, &size) == 0 && size.ws_col != 0)
    columns = size.ws_col;
  if (columns < 20) {
    const struct leme_public_text tiny = columns > 1
                                             ? LEME_PUBLIC_TEXT("\r\x1b[2K>")
                                             : LEME_PUBLIC_TEXT("\r\x1b[2K");
    const enum timao_status status = timao_repl_write(editor, tiny, error);
    if (status == TIMAO_OK)
      editor->visible = true;
    return status;
  }
  if (columns > 512)
    columns = 512;
  const char *prompt = editor->continuation ? "...> " : "timao> ";
  const size_t available = columns - strlen(prompt) - 1;
  const locale_t previous = uselocale(editor->locale);
  if (previous == (locale_t)0)
    return timao_error(error, "io_error", "unable to select terminal locale");
  size_t start = editor->cursor, left = 0, scalars = 0;
  while (start != 0 && scalars < 256) {
    const size_t before = timao_repl_previous(editor, start);
    const struct glyph one = glyph(editor, before);
    if (one.cells > available / 2 - left)
      break;
    left += one.cells;
    start = before;
    ++scalars;
  }
  char bytes[8192] = {0};
  struct leme_json output = {0};
  leme_json_init_fixed(&output, bytes, sizeof(bytes));
  leme_json_append(&output, "\r\x1b[2K", 5);
  leme_json_append(&output, prompt, strlen(prompt));
  size_t cells = 0, cursor_cells = 0, at = start;
  if (start != 0) {
    leme_json_append(&output, "<", 1);
    ++cells;
  }
  scalars = 0;
  while (at < editor->length && scalars < 512) {
    if (at == editor->cursor)
      cursor_cells = cells;
    const struct glyph one = glyph(editor, at);
    if (one.cells > available - cells - 1)
      break;
    leme_json_append(&output, one.bytes, one.length);
    cells += one.cells;
    at += one.consumed;
    ++scalars;
  }
  if (at == editor->cursor)
    cursor_cells = cells;
  if (at < editor->length) {
    leme_json_append(&output, ">", 1);
    ++cells;
  }
  if (cells > cursor_cells) {
    char move[32] = {0};
    const int length =
        snprintf(move, sizeof(move), "\x1b[%zuD", cells - cursor_cells);
    if (length < 0 || (size_t)length >= sizeof(move))
      output.failed = true;
    else
      leme_json_append(&output, move, (size_t)length);
  }
  const bool restored = uselocale(previous) != (locale_t)0;
  if (!restored || output.failed)
    return timao_error(error, "io_error", "unable to render terminal input");
  const enum timao_status status = timao_repl_write(
      editor, (struct leme_public_text){bytes, output.length}, error);
  if (status == TIMAO_OK)
    editor->visible = true;
  return status;
}
