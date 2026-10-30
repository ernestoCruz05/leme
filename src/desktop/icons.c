#include "desktop/icons.h"
#include "desktop/pixels.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wlr/types/wlr_buffer.h>

struct leme_desk_icon {
  struct leme_desk_icon *next;
  char *path;
  int pixels;
  struct stat source;
  struct wlr_buffer *buffer;
};

struct leme_desk_icons {
  struct leme_desk_icon *entries;
};

struct leme_desk_icons *leme_desk_icons_create(void) {
  struct leme_desk_icons *icons = calloc(1, sizeof(*icons));
  return icons;
}

static void leme_desk_icon_destroy(struct leme_desk_icon *icon) {
  wlr_buffer_drop(icon->buffer);
  free(icon->path);
  free(icon);
}

void leme_desk_icons_clear(struct leme_desk_icons *icons) {
  if (icons == NULL) {
    return;
  }
  while (icons->entries != NULL) {
    struct leme_desk_icon *entry = icons->entries;
    icons->entries = entry->next;
    leme_desk_icon_destroy(entry);
  }
}

void leme_desk_icons_destroy(struct leme_desk_icons *icons) {
  if (icons == NULL) {
    return;
  }
  leme_desk_icons_clear(icons);
  free(icons);
}

void leme_desk_icons_prune(struct leme_desk_icons *icons) {
  if (icons == NULL) {
    return;
  }
  struct leme_desk_icon **cursor = &icons->entries;
  while (*cursor != NULL) {
    struct leme_desk_icon *entry = *cursor;
    if (entry->buffer->n_locks == 0) {
      *cursor = entry->next;
      leme_desk_icon_destroy(entry);
    } else {
      cursor = &entry->next;
    }
  }
}

void leme_desk_icons_for_each_path(const struct leme_desk_icons *icons,
                                   void (*visit)(const char *path, void *data),
                                   void *data) {
  if (icons == NULL || visit == NULL) {
    return;
  }
  for (const struct leme_desk_icon *entry = icons->entries; entry != NULL;
       entry = entry->next) {
    visit(entry->path, data);
  }
}

static bool leme_desk_icon_source_matches(const struct stat *left,
                                          const struct stat *right) {
  return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
         left->st_size == right->st_size &&
         left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
         left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
         left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
         left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

struct wlr_buffer *leme_desk_icons_get(struct leme_desk_icons *icons,
                                       const char *path, int pixels) {
  if (icons == NULL || path == NULL) {
    return NULL;
  }
  struct stat source = {0};
  const bool exists = stat(path, &source) == 0 && S_ISREG(source.st_mode);
  struct leme_desk_icon **cursor = &icons->entries;
  while (*cursor != NULL) {
    struct leme_desk_icon *entry = *cursor;
    if (strcmp(entry->path, path) == 0) {
      if (!exists || !leme_desk_icon_source_matches(&entry->source, &source)) {
        *cursor = entry->next;
        leme_desk_icon_destroy(entry);
        continue;
      }
      if (entry->pixels == pixels) {
        return entry->buffer;
      }
    }
    cursor = &entry->next;
  }
  if (!exists) {
    return NULL;
  }
  struct leme_desk_icon *entry = calloc(1, sizeof(*entry));
  if (entry == NULL) {
    return NULL;
  }
  entry->path = strdup(path);
  if (entry->path != NULL) {
    entry->buffer = leme_desk_pixels_load(path, pixels);
  }
  if (entry->buffer == NULL || stat(path, &entry->source) != 0 ||
      !leme_desk_icon_source_matches(&source, &entry->source)) {
    leme_desk_icon_destroy(entry);
    return NULL;
  }
  entry->pixels = pixels;
  entry->next = icons->entries;
  icons->entries = entry;
  return entry->buffer;
}
