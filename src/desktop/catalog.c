#define _XOPEN_SOURCE 700

#include "desktop/catalog.h"

#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct leme_desk_catalog {
  struct leme_desk_entry *entries;
  size_t count;
  size_t capacity;
};

char *leme_desk_catalog_directory(void) {
  const char *home = g_get_home_dir();
  if (home == NULL || home[0] != '/') {
    return NULL;
  }
  g_reload_user_special_dirs_cache();
  const char *configured = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
  char *fallback =
      configured == NULL ? g_build_filename(home, "Desktop", NULL) : NULL;
  const char *directory = configured == NULL ? fallback : configured;
  char *result = NULL;
  if (directory != NULL && directory[0] == '/') {
    char *canonical = g_canonicalize_filename(directory, NULL);
    char *canonical_home = g_canonicalize_filename(home, NULL);
    if (canonical != NULL && canonical_home != NULL &&
        strcmp(canonical, canonical_home) != 0) {
      result = strdup(canonical);
    }
    g_free(canonical_home);
    g_free(canonical);
  }
  g_free(fallback);
  return result;
}

static void leme_desk_entry_finish(struct leme_desk_entry *entry) {
  g_free(entry->path);
  g_free(entry->label);
  g_free(entry->icon);
  *entry = (struct leme_desk_entry){0};
}

static void leme_desk_catalog_finish(struct leme_desk_catalog *catalog) {
  for (size_t i = 0; i < catalog->count; i++) {
    leme_desk_entry_finish(&catalog->entries[i]);
  }
  free(catalog->entries);
  *catalog = (struct leme_desk_catalog){0};
}

struct leme_desk_catalog *leme_desk_catalog_create(void) {
  struct leme_desk_catalog *catalog = calloc(1, sizeof(*catalog));
  return catalog;
}

void leme_desk_catalog_destroy(struct leme_desk_catalog *catalog) {
  if (catalog == NULL) {
    return;
  }
  leme_desk_catalog_finish(catalog);
  free(catalog);
}

char *leme_desk_catalog_launcher_icon(const char *path) {
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return NULL;
  }
  char data[65537] = {0};
  size_t used = 0;
  char *icon = NULL;
  struct stat status = {0};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || status.st_size > 65536) {
    goto cleanup;
  }
  while (used < sizeof(data)) {
    const ssize_t length = read(fd, data + used, sizeof(data) - used);
    if (length < 0) {
      if (errno == EINTR) {
        continue;
      }
      goto cleanup;
    }
    if (length == 0) {
      break;
    }
    used += (size_t)length;
  }
  if (used >= sizeof(data)) {
    goto cleanup;
  }
  GKeyFile *key = g_key_file_new();
  if (key == NULL) {
    goto cleanup;
  }
  if (g_key_file_load_from_data(key, data, used, G_KEY_FILE_NONE, NULL)) {
    icon = g_key_file_get_string(key, "Desktop Entry", "Icon", NULL);
    if (icon != NULL && icon[0] == '\0') {
      g_clear_pointer(&icon, g_free);
    }
  }
  g_key_file_unref(key);
cleanup:
  if (close(fd) != 0) {
    g_clear_pointer(&icon, g_free);
  }
  return icon;
}

static char *leme_desk_file_icon(const char *name) {
  gboolean uncertain = false;
  char *type = g_content_type_guess(name, NULL, 0, &uncertain);
  GIcon *icon = type == NULL ? NULL : g_content_type_get_icon(type);
  char *result = NULL;
  if (icon != NULL && G_IS_THEMED_ICON(icon)) {
    const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (names != NULL && names[0] != NULL) {
      result = strdup(names[0]);
    }
  }
  g_clear_object(&icon);
  g_free(type);
  return result;
}

static bool leme_desk_entry_init(struct leme_desk_entry *entry,
                                 const char *directory, const char *name) {
  *entry = (struct leme_desk_entry){0};
  entry->path = g_build_filename(directory, name, NULL);
  entry->label = g_filename_display_name(name);
  if (entry->path == NULL || entry->label == NULL) {
    goto fail;
  }
  for (char *cursor = entry->label; *cursor != '\0'; cursor++) {
    if ((unsigned char)*cursor < 32 || *cursor == 127) {
      *cursor = ' ';
    }
  }
  struct stat status = {0};
  const bool exists = stat(entry->path, &status) == 0;
  entry->directory = exists && S_ISDIR(status.st_mode);
  if (entry->directory) {
    entry->icon = strdup("folder");
  } else if (exists && S_ISREG(status.st_mode) &&
             g_str_has_suffix(name, ".svg")) {
    entry->icon = strdup(entry->path);
  } else if (exists && S_ISREG(status.st_mode) &&
             g_str_has_suffix(name, ".desktop")) {
    entry->icon = leme_desk_catalog_launcher_icon(entry->path);
  } else {
    entry->icon = leme_desk_file_icon(name);
  }
  if (entry->icon == NULL) {
    entry->icon = strdup(entry->directory ? "folder" : "text-x-generic");
  }
  if (entry->icon != NULL) {
    return true;
  }
fail:
  leme_desk_entry_finish(entry);
  return false;
}

static bool leme_desk_catalog_append(struct leme_desk_catalog *catalog,
                                     const char *directory, const char *name) {
  if (catalog->count == catalog->capacity) {
    if (catalog->capacity > SIZE_MAX / 2) {
      return false;
    }
    const size_t capacity = catalog->capacity == 0 ? 32 : catalog->capacity * 2;
    if (capacity > SIZE_MAX / sizeof(*catalog->entries)) {
      return false;
    }
    struct leme_desk_entry *entries =
        realloc(catalog->entries, capacity * sizeof(*entries));
    if (entries == NULL) {
      return false;
    }
    catalog->entries = entries;
    catalog->capacity = capacity;
  }
  if (!leme_desk_entry_init(&catalog->entries[catalog->count], directory,
                            name)) {
    return false;
  }
  catalog->count++;
  return true;
}

static int leme_desk_entry_compare(const void *left, const void *right) {
  const struct leme_desk_entry *a = left;
  const struct leme_desk_entry *b = right;
  return strcmp(a->path, b->path);
}

bool leme_desk_catalog_scan(struct leme_desk_catalog *catalog,
                            const char *directory) {
  if (catalog == NULL || directory == NULL || directory[0] != '/') {
    return false;
  }
  struct leme_desk_catalog next = {0};
  DIR *stream = opendir(directory);
  if (stream == NULL) {
    if (errno != ENOENT && errno != ENOTDIR) {
      return false;
    }
    leme_desk_catalog_finish(catalog);
    return true;
  }
  bool ok = true;
  while (ok) {
    errno = 0;
    const struct dirent *entry = readdir(stream);
    if (entry == NULL) {
      ok = errno == 0;
      break;
    }
    if (entry->d_name[0] == '.') {
      continue;
    }
    if (!leme_desk_catalog_append(&next, directory, entry->d_name)) {
      ok = false;
      break;
    }
  }
  if (closedir(stream) != 0) {
    ok = false;
  }
  if (!ok) {
    leme_desk_catalog_finish(&next);
    return false;
  }
  if (next.count > 1) {
    qsort(next.entries, next.count, sizeof(*next.entries),
          leme_desk_entry_compare);
  }
  leme_desk_catalog_finish(catalog);
  *catalog = next;
  return true;
}

size_t leme_desk_catalog_count(const struct leme_desk_catalog *catalog) {
  return catalog == NULL ? 0 : catalog->count;
}

const struct leme_desk_entry *
leme_desk_catalog_entry(const struct leme_desk_catalog *catalog, size_t index) {
  return catalog == NULL || index >= catalog->count ? NULL
                                                    : &catalog->entries[index];
}
