#include "desktop/positions.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#define LEME_DESK_POSITIONS_HEADER "leme-desktop-positions 1"
#define LEME_DESK_POSITIONS_DIRECTORY "directory "

struct leme_desk_cell {
  uint32_t column;
  uint32_t row;
};

struct leme_desk_positions {
  char *directory;
  GHashTable *cells;
};

char *leme_desk_positions_path(void) {
  return g_build_filename(g_get_user_state_dir(), "leme", "desktop-positions",
                          NULL);
}

static bool leme_desk_positions_number(const char **text, uint32_t *value) {
  if (!g_ascii_isdigit(**text)) {
    return false;
  }
  char *end = NULL;
  const guint64 parsed = g_ascii_strtoull(*text, &end, 10);
  if (end == NULL || *end != ' ' || parsed > UINT32_MAX) {
    return false;
  }
  *value = (uint32_t)parsed;
  *text = end + 1;
  return true;
}

static void leme_desk_positions_parse(struct leme_desk_positions *positions,
                                      char **lines) {
  if (lines[0] == NULL || strcmp(lines[0], LEME_DESK_POSITIONS_HEADER) != 0 ||
      lines[1] == NULL ||
      !g_str_has_prefix(lines[1], LEME_DESK_POSITIONS_DIRECTORY)) {
    return;
  }
  char *directory =
      g_strcompress(lines[1] + strlen(LEME_DESK_POSITIONS_DIRECTORY));
  const bool same = g_strcmp0(directory, positions->directory) == 0;
  g_free(directory);
  for (size_t i = 2; same && lines[i] != NULL; i++) {
    const char *text = lines[i];
    uint32_t column = 0;
    uint32_t row = 0;
    if (!leme_desk_positions_number(&text, &column) ||
        !leme_desk_positions_number(&text, &row) || *text == '\0') {
      continue;
    }
    char *name = g_strcompress(text);
    (void)leme_desk_positions_set(positions, name, column, row);
    g_free(name);
  }
}

struct leme_desk_positions *leme_desk_positions_load(const char *path,
                                                     const char *directory) {
  struct leme_desk_positions *positions = calloc(1, sizeof(*positions));
  if (positions == NULL) {
    return NULL;
  }
  positions->directory = g_strdup(directory);
  positions->cells =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  char *contents = NULL;
  if (path != NULL && directory != NULL &&
      g_file_get_contents(path, &contents, NULL, NULL)) {
    char **lines = g_strsplit(contents, "\n", -1);
    leme_desk_positions_parse(positions, lines);
    g_strfreev(lines);
    g_free(contents);
  }
  return positions;
}

void leme_desk_positions_destroy(struct leme_desk_positions *positions) {
  if (positions == NULL) {
    return;
  }
  g_hash_table_unref(positions->cells);
  g_free(positions->directory);
  free(positions);
}

bool leme_desk_positions_get(const struct leme_desk_positions *positions,
                             const char *name, uint32_t *column,
                             uint32_t *row) {
  const struct leme_desk_cell *cell =
      positions == NULL ? NULL : g_hash_table_lookup(positions->cells, name);
  if (cell == NULL) {
    return false;
  }
  *column = cell->column;
  *row = cell->row;
  return true;
}

bool leme_desk_positions_set(struct leme_desk_positions *positions,
                             const char *name, uint32_t column, uint32_t row) {
  if (positions == NULL || name == NULL || name[0] == '\0') {
    return false;
  }
  struct leme_desk_cell *cell = g_new(struct leme_desk_cell, 1);
  *cell = (struct leme_desk_cell){.column = column, .row = row};
  g_hash_table_insert(positions->cells, g_strdup(name), cell);
  return true;
}

void leme_desk_positions_forget(struct leme_desk_positions *positions,
                                const char *name) {
  if (positions != NULL && name != NULL) {
    (void)g_hash_table_remove(positions->cells, name);
  }
}

bool leme_desk_positions_rename(struct leme_desk_positions *positions,
                                const char *from, const char *to) {
  uint32_t column = 0;
  uint32_t row = 0;
  if (!leme_desk_positions_get(positions, from, &column, &row)) {
    return false;
  }
  leme_desk_positions_forget(positions, from);
  return leme_desk_positions_set(positions, to, column, row);
}

static int leme_desk_positions_compare(gconstpointer a, gconstpointer b) {
  return strcmp(a, b);
}

bool leme_desk_positions_save(const struct leme_desk_positions *positions,
                              const char *path) {
  if (positions == NULL || positions->directory == NULL || path == NULL) {
    return false;
  }
  GString *text = g_string_new(LEME_DESK_POSITIONS_HEADER "\n");
  char *directory = g_strescape(positions->directory, NULL);
  g_string_append_printf(text, LEME_DESK_POSITIONS_DIRECTORY "%s\n", directory);
  g_free(directory);
  GList *names = g_list_sort(g_hash_table_get_keys(positions->cells),
                             leme_desk_positions_compare);
  for (const GList *link = names; link != NULL; link = link->next) {
    const struct leme_desk_cell *cell =
        g_hash_table_lookup(positions->cells, link->data);
    char *name = g_strescape(link->data, NULL);
    g_string_append_printf(text, "%u %u %s\n", cell->column, cell->row, name);
    g_free(name);
  }
  g_list_free(names);
  char *parent = g_path_get_dirname(path);
  const bool saved =
      g_mkdir_with_parents(parent, 0700) == 0 &&
      g_file_set_contents(path, text->str, (gssize)text->len, NULL);
  g_free(parent);
  g_string_free(text, true);
  return saved;
}
