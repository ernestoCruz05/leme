#include "desktop/theme.h"

#include <glib.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct leme_desk_theme_search {
  GPtrArray *roots;
  GHashTable *visited;
  const char *name;
  int logical_size;
  int scale;
};

static bool leme_desk_theme_component(const char *text) {
  return text != NULL && text[0] != '\0' && strchr(text, '/') == NULL &&
         strcmp(text, ".") != 0 && strcmp(text, "..") != 0;
}

static bool leme_desk_theme_relative(const char *text) {
  if (text == NULL || text[0] == '\0' || text[0] == '/') {
    return false;
  }
  char **parts = g_strsplit(text, "/", -1);
  if (parts == NULL) {
    return false;
  }
  bool valid = true;
  for (size_t i = 0; parts[i] != NULL; i++) {
    if (!leme_desk_theme_component(parts[i])) {
      valid = false;
      break;
    }
  }
  g_strfreev(parts);
  return valid;
}

static bool leme_desk_theme_image(const char *path, char **out) {
  const char *extension = strrchr(path, '.');
  struct stat status = {0};
  if (extension == NULL ||
      (g_ascii_strcasecmp(extension, ".svg") != 0 &&
       g_ascii_strcasecmp(extension, ".png") != 0) ||
      stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
    return false;
  }
  char *canonical = g_canonicalize_filename(path, NULL);
  if (canonical == NULL) {
    return false;
  }
  *out = strdup(canonical);
  g_free(canonical);
  return *out != NULL;
}

static bool leme_desk_theme_candidate(const char *root, const char *theme,
                                      const char *directory, const char *name,
                                      char **out) {
  const bool suffixed =
      g_str_has_suffix(name, ".svg") || g_str_has_suffix(name, ".png");
  const char *const extensions[] = {".svg", ".png"};
  for (size_t i = 0; i < (suffixed ? 1U : 2U); i++) {
    char *filename =
        suffixed ? g_strdup(name) : g_strconcat(name, extensions[i], NULL);
    if (filename == NULL) {
      return false;
    }
    char *candidate = g_build_filename(root, theme, directory, filename, NULL);
    g_free(filename);
    if (candidate == NULL) {
      return false;
    }
    const bool found = leme_desk_theme_image(candidate, out);
    g_free(candidate);
    if (found) {
      return true;
    }
  }
  return false;
}

static int leme_desk_theme_integer(GKeyFile *key, const char *group,
                                   const char *name, int fallback) {
  if (!g_key_file_has_key(key, group, name, NULL)) {
    return fallback;
  }
  GError *error = NULL;
  const int value = g_key_file_get_integer(key, group, name, &error);
  if (error != NULL) {
    g_error_free(error);
    return -1;
  }
  return value;
}

static bool leme_desk_theme_distance(GKeyFile *key, const char *directory,
                                     int size, int scale, int64_t *distance) {
  const int nominal = leme_desk_theme_integer(key, directory, "Size", -1);
  const int factor = leme_desk_theme_integer(key, directory, "Scale", 1);
  if (nominal < 1 || factor < 1) {
    return false;
  }
  int64_t low = nominal;
  int64_t high = nominal;
  char *type = g_key_file_get_string(key, directory, "Type", NULL);
  bool valid = true;
  if (type != NULL && strcmp(type, "Scalable") == 0) {
    low = leme_desk_theme_integer(key, directory, "MinSize", nominal);
    high = leme_desk_theme_integer(key, directory, "MaxSize", nominal);
    valid = low > 0 && high >= low;
  } else if (type == NULL || strcmp(type, "Threshold") == 0) {
    const int threshold =
        leme_desk_theme_integer(key, directory, "Threshold", 2);
    valid = threshold >= 0;
    low -= threshold;
    high += threshold;
    if (low < 0) {
      low = 0;
    }
  } else if (strcmp(type, "Fixed") != 0) {
    valid = false;
  }
  g_free(type);
  if (!valid || high > INT64_MAX / factor) {
    return false;
  }
  low *= factor;
  high *= factor;
  const int64_t target = (int64_t)size * scale;
  *distance = target < low ? low - target : (target > high ? target - high : 0);
  return true;
}

static GKeyFile *
leme_desk_theme_index(const struct leme_desk_theme_search *search,
                      const char *theme) {
  GKeyFile *key = g_key_file_new();
  if (key == NULL) {
    return NULL;
  }
  g_key_file_set_list_separator(key, ',');
  for (guint i = 0; i < search->roots->len; i++) {
    char *path = g_build_filename(g_ptr_array_index(search->roots, i), theme,
                                  "index.theme", NULL);
    if (path == NULL) {
      break;
    }
    const bool loaded =
        g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    if (loaded) {
      return key;
    }
  }
  g_key_file_unref(key);
  return NULL;
}

static bool leme_desk_theme_find(struct leme_desk_theme_search *search,
                                 const char *theme, char **out) {
  if (!leme_desk_theme_component(theme) ||
      g_hash_table_contains(search->visited, theme) ||
      g_hash_table_size(search->visited) >= 64) {
    return false;
  }
  char *visited = g_strdup(theme);
  if (visited == NULL) {
    return false;
  }
  g_hash_table_add(search->visited, visited);
  GKeyFile *key = leme_desk_theme_index(search, theme);
  if (key == NULL) {
    return false;
  }
  char *best = NULL;
  int64_t best_distance = INT64_MAX;
  const char *const lists[] = {"Directories", "ScaledDirectories"};
  for (size_t list = 0; list < 2; list++) {
    gsize count = 0;
    char **directories = g_key_file_get_string_list(key, "Icon Theme",
                                                    lists[list], &count, NULL);
    for (size_t i = 0; directories != NULL && i < count; i++) {
      char *directory = g_strstrip(directories[i]);
      int64_t distance = 0;
      if (!leme_desk_theme_relative(directory) ||
          !leme_desk_theme_distance(key, directory, search->logical_size,
                                    search->scale, &distance) ||
          distance >= best_distance) {
        continue;
      }
      for (guint root = 0; root < search->roots->len; root++) {
        char *candidate = NULL;
        if (leme_desk_theme_candidate(g_ptr_array_index(search->roots, root),
                                      theme, directory, search->name,
                                      &candidate)) {
          free(best);
          best = candidate;
          best_distance = distance;
          break;
        }
      }
    }
    g_strfreev(directories);
  }
  if (best != NULL) {
    *out = best;
    g_key_file_unref(key);
    return true;
  }
  gsize count = 0;
  char **parents =
      g_key_file_get_string_list(key, "Icon Theme", "Inherits", &count, NULL);
  bool found = false;
  for (size_t i = 0; parents != NULL && i < count; i++) {
    if (leme_desk_theme_find(search, g_strstrip(parents[i]), out)) {
      found = true;
      break;
    }
  }
  g_strfreev(parents);
  g_key_file_unref(key);
  return found;
}

static void leme_desk_theme_roots(GPtrArray *roots) {
  g_ptr_array_add(roots,
                  g_build_filename(g_get_user_data_dir(), "icons", NULL));
  g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".icons", NULL));
  const char *const *system = g_get_system_data_dirs();
  for (size_t i = 0; system[i] != NULL; i++) {
    g_ptr_array_add(roots, g_build_filename(system[i], "icons", NULL));
  }
}

bool leme_desk_theme_installed(const char *theme) {
  if (!leme_desk_theme_component(theme)) {
    return false;
  }
  GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
  if (roots == NULL) {
    return false;
  }
  leme_desk_theme_roots(roots);
  bool installed = false;
  for (guint i = 0; i < roots->len && !installed; i++) {
    char *path = g_build_filename(g_ptr_array_index(roots, i), theme,
                                  "index.theme", NULL);
    installed = path != NULL && access(path, R_OK) == 0;
    g_free(path);
  }
  g_ptr_array_unref(roots);
  return installed;
}

bool leme_desk_theme_resolve(const char *theme, const char *name,
                             int logical_size, int scale, char **path) {
  if (path == NULL || *path != NULL || name == NULL || logical_size < 1 ||
      logical_size > 4096 || scale < 1 || scale > 16) {
    return false;
  }
  if (name[0] == '/') {
    return leme_desk_theme_image(name, path);
  }
  if (!leme_desk_theme_component(name) || !leme_desk_theme_component(theme)) {
    return false;
  }
  struct leme_desk_theme_search search = {
      .roots = g_ptr_array_new_with_free_func(g_free),
      .visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
      .name = name,
      .logical_size = logical_size,
      .scale = scale,
  };
  if (search.roots == NULL || search.visited == NULL) {
    if (search.roots != NULL) {
      g_ptr_array_unref(search.roots);
    }
    if (search.visited != NULL) {
      g_hash_table_unref(search.visited);
    }
    return false;
  }
  leme_desk_theme_roots(search.roots);
  const char *const *system = g_get_system_data_dirs();
  bool found = leme_desk_theme_find(&search, theme, path) ||
               leme_desk_theme_find(&search, "hicolor", path);
  if (!found) {
    for (guint i = 0; i < search.roots->len && !found; i++) {
      found = leme_desk_theme_candidate(g_ptr_array_index(search.roots, i), "",
                                        "", name, path);
    }
  }
  for (size_t i = 0; system[i] != NULL && !found; i++) {
    found = leme_desk_theme_candidate(system[i], "pixmaps", "", name, path);
  }
  g_hash_table_unref(search.visited);
  g_ptr_array_unref(search.roots);
  return found;
}
