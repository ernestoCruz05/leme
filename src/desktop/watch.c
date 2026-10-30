#define _XOPEN_SOURCE 700
#include "desktop/watch.h"

#include <glib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

struct leme_desk_watch_entry {
  int descriptor;
  char *name;
  bool fixed;
  bool used;
};

struct leme_desk_watch {
  int fd;
  char *directory;
  struct wl_array entries;
  struct wl_event_source *source;
  struct wl_event_source *timer;
  void (*changed)(void *data);
  void *data;
};

static const uint32_t leme_desk_watch_mask =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
    IN_MOVE_SELF | IN_CLOSE_WRITE | IN_ATTRIB;

bool leme_desk_watch_changes(uint32_t mask) {
  return (mask & (leme_desk_watch_mask | IN_Q_OVERFLOW | IN_UNMOUNT)) != 0;
}

static size_t leme_desk_watch_count(const struct leme_desk_watch *watch) {
  return watch->entries.size / sizeof(struct leme_desk_watch_entry);
}

static bool leme_desk_watch_shared(const struct leme_desk_watch *watch,
                                   int descriptor) {
  const struct leme_desk_watch_entry *entry = NULL;
  wl_array_for_each(entry, &watch->entries) {
    if (entry->descriptor == descriptor) {
      return true;
    }
  }
  return false;
}

static void leme_desk_watch_remove(struct leme_desk_watch *watch, size_t index,
                                   bool unwatch) {
  struct leme_desk_watch_entry *entries = watch->entries.data;
  const int descriptor = entries[index].descriptor;
  free(entries[index].name);
  entries[index] = entries[leme_desk_watch_count(watch) - 1];
  watch->entries.size -= sizeof(*entries);
  if (unwatch && !leme_desk_watch_shared(watch, descriptor) &&
      inotify_rm_watch(watch->fd, descriptor) < 0 && errno != EINVAL) {
    wlr_log_errno(WLR_ERROR, "%s", "leme: failed to remove desktop watch");
  }
}

static void leme_desk_watch_prune(struct leme_desk_watch *watch, bool fixed) {
  const struct leme_desk_watch_entry *entries = watch->entries.data;
  for (size_t i = 0; i < leme_desk_watch_count(watch);) {
    if (fixed ? entries[i].fixed : !entries[i].fixed && !entries[i].used) {
      leme_desk_watch_remove(watch, i, true);
    } else {
      i++;
    }
  }
}

static bool leme_desk_watch_add(struct leme_desk_watch *watch,
                                const char *directory, const char *name,
                                bool fixed) {
  const int descriptor = inotify_add_watch(
      watch->fd, directory, leme_desk_watch_mask | IN_ONLYDIR | IN_MASK_ADD);
  if (descriptor < 0) {
    return false;
  }
  struct leme_desk_watch_entry *entry = NULL;
  wl_array_for_each(entry, &watch->entries) {
    if (entry->descriptor == descriptor && entry->fixed == fixed &&
        g_strcmp0(entry->name, name) == 0) {
      entry->used = true;
      return true;
    }
  }
  char *copy = name == NULL ? NULL : strdup(name);
  entry = name != NULL && copy == NULL
              ? NULL
              : wl_array_add(&watch->entries, sizeof(*entry));
  if (entry == NULL) {
    free(copy);
    return false;
  }
  *entry = (struct leme_desk_watch_entry){
      .descriptor = descriptor, .name = copy, .fixed = fixed, .used = true};
  return true;
}

void leme_desk_watch_begin_files(struct leme_desk_watch *watch) {
  if (watch == NULL) {
    return;
  }
  struct leme_desk_watch_entry *entry = NULL;
  wl_array_for_each(entry, &watch->entries) { entry->used = entry->fixed; }
}

static char *leme_desk_watch_target(const char *path) {
  char *real = realpath(path, NULL);
  if (real != NULL) {
    char *target = g_strdup(real);
    free(real);
    return target;
  }
  char *link = g_file_read_link(path, NULL);
  if (link == NULL) {
    return NULL;
  }
  char *base = g_path_get_dirname(path);
  char *target = g_canonicalize_filename(link, base);
  g_free(base);
  g_free(link);
  return target;
}

bool leme_desk_watch_add_file(struct leme_desk_watch *watch, const char *path) {
  if (watch == NULL || watch->fd < 0 || path == NULL || path[0] != '/') {
    return false;
  }
  char *resolved = leme_desk_watch_target(path);
  const char *const paths[] = {path, resolved};
  bool added = false;
  for (size_t i = 0; i < 2; i++) {
    if (paths[i] == NULL || (i == 1 && strcmp(paths[0], paths[1]) == 0)) {
      continue;
    }
    char *directory = g_path_get_dirname(paths[i]);
    char *name = g_path_get_basename(paths[i]);
    added = leme_desk_watch_add(watch, directory, name, false) || added;
    g_free(name);
    g_free(directory);
  }
  g_free(resolved);
  return added;
}

void leme_desk_watch_end_files(struct leme_desk_watch *watch) {
  if (watch != NULL) {
    leme_desk_watch_prune(watch, false);
  }
}

static bool leme_desk_watch_arm(struct leme_desk_watch *watch) {
  leme_desk_watch_prune(watch, true);
  char *parent = g_path_get_dirname(watch->directory);
  char *name = g_path_get_basename(watch->directory);
  const bool watched = leme_desk_watch_add(watch, watch->directory, NULL, true);
  const bool parented = leme_desk_watch_add(watch, parent, name, true);
  g_free(name);
  g_free(parent);
  if (!watched && !parented) {
    wlr_log(WLR_ERROR, "leme: cannot watch desktop directory %s",
            watch->directory);
    return false;
  }
  return true;
}

static void leme_desk_watch_close(struct leme_desk_watch *watch) {
  if (watch->source != NULL) {
    (void)wl_event_source_remove(watch->source);
    watch->source = NULL;
  }
  if (watch->timer != NULL) {
    (void)wl_event_source_remove(watch->timer);
    watch->timer = NULL;
  }
  if (watch->fd >= 0) {
    if (close(watch->fd) != 0) {
      wlr_log_errno(WLR_ERROR, "%s", "leme: could not close desktop watch");
    }
    watch->fd = -1;
  }
}

static int leme_desk_watch_handle_timer(void *data) {
  struct leme_desk_watch *watch = data;
  (void)leme_desk_watch_arm(watch);
  watch->changed(watch->data);
  return 0;
}

static bool leme_desk_watch_event(struct leme_desk_watch *watch,
                                  const struct inotify_event *event,
                                  const char *name) {
  const struct leme_desk_watch_entry *entries = watch->entries.data;
  bool matched = false;
  if ((event->mask & IN_IGNORED) != 0) {
    for (size_t i = 0; i < leme_desk_watch_count(watch);) {
      if (entries[i].descriptor == event->wd) {
        leme_desk_watch_remove(watch, i, false);
        matched = true;
      } else {
        i++;
      }
    }
    return matched;
  }
  if ((event->mask & IN_Q_OVERFLOW) != 0) {
    return true;
  }
  if (!leme_desk_watch_changes(event->mask)) {
    return false;
  }
  const bool self =
      (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0;
  for (size_t i = 0; i < leme_desk_watch_count(watch) && !matched; i++) {
    matched = entries[i].descriptor == event->wd &&
              (entries[i].name == NULL || self ||
               (event->len > 0 && strcmp(name, entries[i].name) == 0));
  }
  return matched;
}

static int leme_desk_watch_handle_ready(int fd, uint32_t mask, void *data) {
  struct leme_desk_watch *watch = data;
  if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
    wlr_log(WLR_ERROR, "%s", "leme: desktop directory watch stopped");
    leme_desk_watch_close(watch);
    return 0;
  }
  bool changed = false;
  for (;;) {
    char bytes[4096] = {0};
    const ssize_t count = read(fd, bytes, sizeof(bytes));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      wlr_log_errno(WLR_ERROR, "%s",
                    "leme: failed to read desktop directory events");
      leme_desk_watch_close(watch);
      return 0;
    }
    if (count == 0) {
      leme_desk_watch_close(watch);
      return 0;
    }
    const size_t length = (size_t)count;
    size_t offset = 0;
    while (length - offset >= sizeof(struct inotify_event)) {
      struct inotify_event event = {0};
      memcpy(&event, bytes + offset, sizeof(event));
      offset += sizeof(event);
      if (event.len > length - offset) {
        changed = true;
        break;
      }
      changed = leme_desk_watch_event(watch, &event, bytes + offset) || changed;
      offset += event.len;
    }
  }
  if (changed && wl_event_source_timer_update(watch->timer, 100) < 0) {
    wlr_log_errno(WLR_ERROR, "%s",
                  "leme: failed to debounce desktop directory changes");
  }
  return 0;
}

struct leme_desk_watch *leme_desk_watch_create(struct wl_event_loop *loop,
                                               const char *directory,
                                               void (*changed)(void *data),
                                               void *data) {
  if (loop == NULL || directory == NULL || directory[0] != '/' ||
      changed == NULL) {
    return NULL;
  }
  struct leme_desk_watch *watch = calloc(1, sizeof(*watch));
  if (watch == NULL) {
    return NULL;
  }
  watch->fd = -1;
  wl_array_init(&watch->entries);
  watch->directory = strdup(directory);
  watch->changed = changed;
  watch->data = data;
  if (watch->directory == NULL) {
    leme_desk_watch_destroy(watch);
    return NULL;
  }
  watch->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (watch->fd < 0) {
    wlr_log_errno(WLR_ERROR, "%s",
                  "leme: could not create desktop directory watch");
    leme_desk_watch_destroy(watch);
    return NULL;
  }
  watch->source = wl_event_loop_add_fd(loop, watch->fd, WL_EVENT_READABLE,
                                       leme_desk_watch_handle_ready, watch);
  watch->timer =
      wl_event_loop_add_timer(loop, leme_desk_watch_handle_timer, watch);
  if (watch->source == NULL || watch->timer == NULL ||
      !leme_desk_watch_arm(watch)) {
    leme_desk_watch_destroy(watch);
    return NULL;
  }
  return watch;
}

void leme_desk_watch_destroy(struct leme_desk_watch *watch) {
  if (watch == NULL) {
    return;
  }
  leme_desk_watch_close(watch);
  struct leme_desk_watch_entry *entry = NULL;
  wl_array_for_each(entry, &watch->entries) { free(entry->name); }
  wl_array_release(&watch->entries);
  free(watch->directory);
  free(watch);
}
