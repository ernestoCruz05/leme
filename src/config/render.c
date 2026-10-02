#include "config/render.h"
#include "config/diagnostics.h"
#include "config/expand.h"
#include "config/source_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool leme_render_append(char **buffer, size_t *length, const char *text,
                               size_t text_length) {
  char *grown = realloc(*buffer, *length + text_length + 1);

  if (grown == NULL) {
    return false;
  }
  memcpy(grown + *length, text, text_length);
  *buffer = grown;
  *length += text_length;
  (*buffer)[*length] = '\0';
  return true;
}

static bool leme_render_append_str(char **buffer, size_t *length,
                                   const char *text) {
  if (text == NULL) {
    return true;
  }
  return leme_render_append(buffer, length, text, strlen(text));
}

#define LEME_RENDER_LINE_MAX 120

struct leme_render_window {
  size_t start;
  size_t end;
  size_t length;
};

static bool leme_render_continuation(const char *text, size_t index) {
  return ((unsigned char)text[index] & 0xc0) == 0x80;
}

static struct leme_render_window
leme_render_window_for(const char *text, size_t length, size_t focus) {
  struct leme_render_window window = {
      .start = 0, .end = length, .length = length};

  if (length <= LEME_RENDER_LINE_MAX) {
    return window;
  }
  window.start =
      focus > LEME_RENDER_LINE_MAX / 2 ? focus - LEME_RENDER_LINE_MAX / 2 : 0;
  while (window.start < length &&
         leme_render_continuation(text, window.start)) {
    window.start++;
  }
  window.end = length - window.start > LEME_RENDER_LINE_MAX
                   ? window.start + LEME_RENDER_LINE_MAX
                   : length;
  while (window.end < length && window.end > window.start &&
         leme_render_continuation(text, window.end)) {
    window.end--;
  }
  return window;
}

static size_t leme_render_glyph(const char *text, size_t end, size_t index,
                                char glyph[16], size_t *width) {
  const unsigned char byte = (unsigned char)text[index];
  size_t advance = 1;

  *width = 1;
  if (byte == '\t' || (byte >= 0x20 && byte < 0x7f)) {
    glyph[0] = (char)byte;
    glyph[1] = '\0';
    return 1;
  }
  if (byte < 0x20 || byte == 0x7f) {
    (void)snprintf(glyph, 16, "\\x%02x", (unsigned int)byte);
    *width = 4;
    return 1;
  }
  if (end - index >= 3 && memcmp(text + index, "\xef\xbb\xbf", 3) == 0) {
    (void)snprintf(glyph, 16, "%s", "<U+FEFF>");
    *width = 8;
    return 3;
  }
  if (end - index >= 3 && memcmp(text + index, "\xe2\x80\x8b", 3) == 0) {
    (void)snprintf(glyph, 16, "%s", "<U+200B>");
    *width = 8;
    return 3;
  }
  if (byte >= 0xf0) {
    advance = 4;
  } else if (byte >= 0xe0) {
    advance = 3;
  } else if (byte >= 0xc0) {
    advance = 2;
  }
  if (advance > end - index) {
    advance = end - index;
  }
  memcpy(glyph, text + index, advance);
  glyph[advance] = '\0';
  return advance;
}

static bool leme_render_line(char **buffer, size_t *length, const char *text,
                             struct leme_render_window window) {
  char glyph[16];
  size_t width;

  if (window.start > 0 && !leme_render_append_str(buffer, length, "...")) {
    return false;
  }
  for (size_t index = window.start; index < window.end;) {
    index += leme_render_glyph(text, window.end, index, glyph, &width);
    if (!leme_render_append_str(buffer, length, glyph)) {
      return false;
    }
  }
  return window.end == window.length ||
         leme_render_append_str(buffer, length, "...");
}

static bool leme_render_padding(char **buffer, size_t *length, const char *text,
                                struct leme_render_window window,
                                size_t focus) {
  char glyph[16];
  size_t width;

  if (window.start > 0 && !leme_render_append_str(buffer, length, "   ")) {
    return false;
  }
  for (size_t index = window.start; index < focus && index < window.end;) {
    const char *pad = text[index] == '\t' ? "\t" : " ";

    index += leme_render_glyph(text, window.end, index, glyph, &width);
    for (size_t column = 0; column < width; column++) {
      if (!leme_render_append_str(buffer, length, pad)) {
        return false;
      }
    }
  }
  return true;
}

static size_t leme_render_width(const char *text,
                                struct leme_render_window window, size_t from,
                                size_t count) {
  const size_t until = count < window.end - from ? from + count : window.end;
  char glyph[16];
  size_t width;
  size_t total = 0;

  for (size_t index = from; index < until;) {
    index += leme_render_glyph(text, window.end, index, glyph, &width);
    total += width;
  }
  return total;
}

static char *
leme_render_format_iterations(const struct leme_trail_table *trails,
                              const uint16_t *iterations, size_t count) {
  char *result = NULL;
  size_t length = 0;
  size_t index;

  for (index = 0; index < count; index++) {
    const struct leme_trail_node *node =
        leme_trail_table_get(trails, iterations[index]);
    const char *val = (node != NULL && node->value != NULL) ? node->value : "";

    if (index > 0) {
      if (count == 2) {
        if (!leme_render_append_str(&result, &length, " and ")) {
          free(result);
          return NULL;
        }
      } else if (index == count - 1) {
        if (!leme_render_append_str(&result, &length, ", and ")) {
          free(result);
          return NULL;
        }
      } else {
        if (!leme_render_append_str(&result, &length, ", ")) {
          free(result);
          return NULL;
        }
      }
    }
    if (!leme_render_append_str(&result, &length, "\"") ||
        !leme_render_append_str(&result, &length, val) ||
        !leme_render_append_str(&result, &length, "\"")) {
      free(result);
      return NULL;
    }
  }
  return result;
}

struct leme_render_span {
  const struct leme_scfg_source *source;
  int line;
  int column;
  size_t length;
  const char *label;
  bool is_primary;
};

char *leme_diagnostic_render_rich_tables(
    const struct leme_source_table *sources,
    const struct leme_scfg_source *source_fallback,
    const struct leme_trail_table *trails, const char *default_path,
    const struct leme_diagnostic *diagnostic, bool colour) {
  char *buffer = NULL;
  size_t length = 0;
  char piece[512];
  int digits = 1;
  const struct leme_scfg_source *primary_source = source_fallback;
  const char *primary_path = default_path;
  int primary_line = diagnostic->line;
  int primary_col = 1;
  size_t index;
  uint16_t curr_trail;
  bool innermost_trail = true;
  struct leme_render_span spans[16];
  size_t span_count = 0;
  int max_line = 1;

  if (sources != NULL) {
    const struct leme_scfg_source *found =
        leme_source_table_get(sources, diagnostic->primary.source);
    if (found != NULL) {
      primary_source = found;
    }
    const char *tbl_path =
        leme_source_table_path(sources, diagnostic->primary.source);
    if (tbl_path != NULL) {
      primary_path = tbl_path;
    }
  }
  if (primary_path == NULL) {
    primary_path = "config";
  }

  if (primary_source != NULL && diagnostic->primary.span.length > 0) {
    leme_scfg_source_locate(primary_source, diagnostic->primary.span,
                            &primary_line, &primary_col);
  }
  if (primary_line == 0) {
    primary_line = diagnostic->line;
  }
  max_line = primary_line;

  if (diagnostic->primary.span.length > 0) {
    spans[span_count++] = (struct leme_render_span){
        .source = primary_source,
        .line = primary_line,
        .column = primary_col,
        .length = diagnostic->primary.span.length,
        .label = diagnostic->primary.label,
        .is_primary = true,
    };
  }

  for (index = 0; index < diagnostic->secondary_count; index++) {
    if (diagnostic->secondary[index].span.length > 0 &&
        span_count < sizeof(spans) / sizeof(spans[0])) {
      const struct leme_scfg_source *sec_source = primary_source;
      int sec_line = 0;
      int sec_col = 0;

      if (sources != NULL) {
        const struct leme_scfg_source *found =
            leme_source_table_get(sources, diagnostic->secondary[index].source);
        if (found != NULL) {
          sec_source = found;
        }
      }
      if (sec_source != NULL) {
        leme_scfg_source_locate(sec_source, diagnostic->secondary[index].span,
                                &sec_line, &sec_col);
      }
      if (sec_line == 0) {
        sec_line = diagnostic->line;
        sec_col = 1;
      }
      if (sec_line > max_line) {
        max_line = sec_line;
      }
      spans[span_count++] = (struct leme_render_span){
          .source = sec_source,
          .line = sec_line,
          .column = sec_col,
          .length = diagnostic->secondary[index].span.length,
          .label = diagnostic->secondary[index].label,
          .is_primary = false,
      };
    }
  }

  /* Sort spans in source reading order: line ascending, column ascending */
  for (size_t i = 0; i < span_count; i++) {
    for (size_t j = i + 1; j < span_count; j++) {
      if (spans[j].line < spans[i].line ||
          (spans[j].line == spans[i].line &&
           spans[j].column < spans[i].column)) {
        struct leme_render_span temp = spans[i];
        spans[i] = spans[j];
        spans[j] = temp;
      }
    }
  }

  for (int temp = max_line; temp >= 10; temp /= 10) {
    digits++;
  }

  /* 1. Header: severity: message */
  if (colour) {
    snprintf(
        piece, sizeof(piece), "%s%s\033[0m: %s\n",
        diagnostic->severity == LEME_DIAGNOSTIC_ERROR ? "\033[31m" : "\033[33m",
        diagnostic->severity == LEME_DIAGNOSTIC_ERROR ? "error" : "warning",
        diagnostic->message != NULL ? diagnostic->message : "");
  } else {
    snprintf(piece, sizeof(piece), "%s: %s\n",
             diagnostic->severity == LEME_DIAGNOSTIC_ERROR ? "error"
                                                           : "warning",
             diagnostic->message != NULL ? diagnostic->message : "");
  }
  if (!leme_render_append_str(&buffer, &length, piece)) {
    goto fail;
  }

  /* 2-6. Source snippet block */
  if (span_count > 0) {
    snprintf(piece, sizeof(piece), "  --> %s:%d:%d\n", primary_path,
             primary_line, primary_col);
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }
    snprintf(piece, sizeof(piece), "%*s|\n", digits + 2, "");
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }

    for (size_t k = 0; k < span_count; k++) {
      const char *line_text = "";
      size_t line_length = 0;
      struct leme_render_window window;
      size_t focus;
      size_t underline_len;
      const char *underline_char;

      if (k > 0) {
        int prev_line = spans[k - 1].line;
        int curr_line = spans[k].line;
        if (curr_line > prev_line + 1) {
          snprintf(piece, sizeof(piece), "%*s|\n", digits + 2, "");
          if (!leme_render_append_str(&buffer, &length, piece)) {
            goto fail;
          }
        }
      }

      if (spans[k].source != NULL) {
        leme_scfg_source_line_text(spans[k].source, spans[k].line, &line_text,
                                   &line_length);
      }
      while (line_length > 0 && (line_text[line_length - 1] == '\n' ||
                                 line_text[line_length - 1] == '\r')) {
        line_length--;
      }

      focus = spans[k].column > 0 ? (size_t)(spans[k].column - 1) : 0;
      if (focus > line_length) {
        focus = line_length;
      }
      window = leme_render_window_for(line_text, line_length, focus);
      snprintf(piece, sizeof(piece), " %*d | ", digits, spans[k].line);
      if (!leme_render_append_str(&buffer, &length, piece) ||
          !leme_render_line(&buffer, &length, line_text, window) ||
          !leme_render_append_str(&buffer, &length, "\n")) {
        goto fail;
      }

      snprintf(piece, sizeof(piece), "%*s| ", digits + 2, "");
      if (!leme_render_append_str(&buffer, &length, piece) ||
          !leme_render_padding(&buffer, &length, line_text, window, focus)) {
        goto fail;
      }
      underline_len = leme_render_width(
          line_text, window, focus, spans[k].length > 0 ? spans[k].length : 1);
      if (underline_len == 0) {
        underline_len = 1;
      }
      underline_char = spans[k].is_primary ? "^" : "-";
      if (colour) {
        if (!leme_render_append_str(
                &buffer, &length,
                spans[k].is_primary
                    ? (diagnostic->severity == LEME_DIAGNOSTIC_ERROR
                           ? "\033[31m"
                           : "\033[33m")
                    : "\033[34m")) {
          goto fail;
        }
      }
      for (size_t u = 0; u < underline_len; u++) {
        if (!leme_render_append_str(&buffer, &length, underline_char)) {
          goto fail;
        }
      }
      if (colour) {
        if (!leme_render_append_str(&buffer, &length, "\033[0m")) {
          goto fail;
        }
      }
      if (spans[k].label != NULL) {
        if (!leme_render_append_str(&buffer, &length, " ") ||
            !leme_render_append_str(&buffer, &length, spans[k].label)) {
          goto fail;
        }
      }
      if (!leme_render_append_str(&buffer, &length, "\n")) {
        goto fail;
      }
    }
  }

  /* 7-9. Notes, Trails, and Helps */
  bool has_notes_or_helps =
      diagnostic->trail != 0 || diagnostic->iterations_count > 0 ||
      diagnostic->notes_count > 0 || diagnostic->helps_count > 0;

  if (has_notes_or_helps && diagnostic->primary.span.length > 0) {
    snprintf(piece, sizeof(piece), "%*s|\n", digits + 2, "");
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }
  }

  /* Trail notes */
  curr_trail = diagnostic->trail;
  while (curr_trail != 0 && trails != NULL) {
    const struct leme_trail_node *node =
        leme_trail_table_get(trails, curr_trail);
    if (node == NULL) {
      break;
    }
    if (innermost_trail && diagnostic->iterations_count > 0) {
      if (diagnostic->iterations_count == 1) {
        const struct leme_trail_node *iter_node =
            leme_trail_table_get(trails, diagnostic->iterations[0]);
        const char *var = (iter_node != NULL && iter_node->variable != NULL)
                              ? iter_node->variable
                              : node->variable;
        const char *val = (iter_node != NULL && iter_node->value != NULL)
                              ? iter_node->value
                              : node->value;
        snprintf(piece, sizeof(piece),
                 "%*s= note: expanded from `for %s`, iteration \"%s\"\n",
                 digits + 2, "", var != NULL ? var : "",
                 val != NULL ? val : "");
      } else {
        char *formatted_iters = leme_render_format_iterations(
            trails, diagnostic->iterations, diagnostic->iterations_count);
        snprintf(piece, sizeof(piece),
                 "%*s= note: expanded from `for %s`, iterations %s\n",
                 digits + 2, "", node->variable != NULL ? node->variable : "",
                 formatted_iters != NULL ? formatted_iters : "");
        free(formatted_iters);
      }
    } else {
      if (node->kind == LEME_TRAIL_FOR) {
        snprintf(piece, sizeof(piece),
                 "%*s= note: expanded from `for %s`, iteration \"%s\"\n",
                 digits + 2, "", node->variable != NULL ? node->variable : "",
                 node->value != NULL ? node->value : "");
      } else if (node->kind == LEME_TRAIL_IF) {
        snprintf(piece, sizeof(piece),
                 "%*s= note: expanded from the %s branch of `if %s`\n",
                 digits + 2, "", node->value != NULL ? node->value : "",
                 node->variable != NULL ? node->variable : "");
      }
    }
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }
    innermost_trail = false;
    curr_trail = node->parent;
  }

  /* Diagnostic notes */
  for (index = 0; index < diagnostic->notes_count; index++) {
    snprintf(piece, sizeof(piece), "%*s= note: %s\n", digits + 2, "",
             diagnostic->notes[index]);
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }
  }

  /* Helps */
  for (index = 0; index < diagnostic->helps_count; index++) {
    snprintf(piece, sizeof(piece), "%*s= help: %s\n", digits + 2, "",
             diagnostic->helps[index]);
    if (!leme_render_append_str(&buffer, &length, piece)) {
      goto fail;
    }
  }

  return buffer;

fail:
  free(buffer);
  return NULL;
}

char *leme_diagnostic_render_rich(const struct leme_config *config,
                                  const struct leme_diagnostic *diagnostic,
                                  bool colour) {
  if (config == NULL || diagnostic == NULL) {
    return NULL;
  }
  return leme_diagnostic_render_rich_tables(&config->sources, NULL,
                                            &config->trails, config->path,
                                            diagnostic, colour);
}

char *leme_diagnostic_render_compact(const struct leme_config *config,
                                     const struct leme_diagnostic *diagnostic) {
  char *buffer = NULL;
  size_t length = 0;
  char piece[512];
  const char *path = NULL;
  uint16_t curr_trail;
  bool first_trail = true;

  if (config == NULL || diagnostic == NULL) {
    return NULL;
  }
  path = leme_source_table_path(&config->sources, diagnostic->primary.source);
  if (path == NULL) {
    path = config->path;
  }
  if (path == NULL) {
    path = "config";
  }

  snprintf(piece, sizeof(piece), "%s:%d: %s", path, diagnostic->line,
           diagnostic->message != NULL ? diagnostic->message : "");
  if (!leme_render_append_str(&buffer, &length, piece)) {
    return NULL;
  }

  curr_trail = diagnostic->trail;
  if (curr_trail != 0) {
    if (!leme_render_append_str(&buffer, &length, " (")) {
      free(buffer);
      return NULL;
    }
    while (curr_trail != 0) {
      const struct leme_trail_node *node =
          leme_trail_table_get(&config->trails, curr_trail);
      if (node == NULL) {
        break;
      }
      if (!first_trail) {
        if (!leme_render_append_str(&buffer, &length, ", ")) {
          free(buffer);
          return NULL;
        }
      }
      if (node->kind == LEME_TRAIL_FOR) {
        snprintf(piece, sizeof(piece), "for %s = \"%s\"",
                 node->variable != NULL ? node->variable : "",
                 node->value != NULL ? node->value : "");
      } else if (node->kind == LEME_TRAIL_IF) {
        snprintf(piece, sizeof(piece), "if %s == \"%s\"",
                 node->variable != NULL ? node->variable : "",
                 node->value != NULL ? node->value : "");
      }
      if (!leme_render_append_str(&buffer, &length, piece)) {
        free(buffer);
        return NULL;
      }
      first_trail = false;
      curr_trail = node->parent;
    }
    if (!leme_render_append_str(&buffer, &length, ")")) {
      free(buffer);
      return NULL;
    }
  }

  return buffer;
}
