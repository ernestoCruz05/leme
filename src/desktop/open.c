#define _XOPEN_SOURCE 700

#include "desktop/open.h"

#include "core/process.h"

#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static bool leme_desk_open_launcher(const char *name, const unsigned char *data,
                                    size_t length) {
  char *type = g_content_type_guess(name, data, length, NULL);
  const bool launcher =
      type == NULL || g_content_type_is_a(type, "application/x-desktop");
  g_free(type);
  return launcher;
}

static const char *leme_desk_open_refusal(const char *path,
                                          const char *resolved) {
  struct stat status = {0};
  if (stat(resolved, &status) != 0) {
    return "cannot inspect target";
  }
  if (S_ISDIR(status.st_mode)) {
    return NULL;
  }
  if (!S_ISREG(status.st_mode)) {
    return "special files cannot be opened from the desktop";
  }
  if ((status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
    return "executable files are disabled";
  }
  unsigned char data[4096] = {0};
  ssize_t used = -1;
  const int fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    do {
      used = read(fd, data, sizeof(data));
    } while (used < 0 && errno == EINTR);
    (void)close(fd);
  }
  if (used < 0) {
    return "cannot read target";
  }
  return leme_desk_open_launcher(path, NULL, 0) ||
                 leme_desk_open_launcher(resolved, NULL, 0) ||
                 leme_desk_open_launcher(NULL, data, (size_t)used)
             ? "desktop launchers are disabled"
             : NULL;
}

bool leme_desk_open_path(struct leme_server *server, const char *path) {
  if (server == NULL || path == NULL || path[0] != '/') {
    wlr_log(WLR_ERROR, "%s", "leme: desktop opening requires an absolute path");
    return false;
  }
  char *resolved = realpath(path, NULL);
  if (resolved == NULL) {
    wlr_log(WLR_ERROR, "leme: cannot resolve desktop entry %s: %s", path,
            strerror(errno));
    return false;
  }
  const char *reason = leme_desk_open_refusal(path, resolved);
  bool opened = false;
  if (reason == NULL) {
    char opener[] = "xdg-open";
    char *argv[] = {opener, resolved, NULL};
    opened = leme_process_spawn_detached(server, argv);
    if (!opened) {
      reason = "cannot start xdg-open; check xdg-utils and the configured PATH";
    }
  }
  if (reason != NULL) {
    wlr_log(WLR_ERROR, "leme: cannot open desktop entry %s: %s", path, reason);
  }
  free(resolved);
  return opened;
}

char **leme_desk_open_expand(char *const *argv, const char *directory,
                             char *const *names, size_t count) {
  if (argv == NULL || directory == NULL || (count > 0 && names == NULL)) {
    return NULL;
  }
  GStrvBuilder *builder = g_strv_builder_new();
  for (size_t i = 0; argv[i] != NULL; i++) {
    if (strcmp(argv[i], "%files") == 0) {
      for (size_t name = 0; name < count; name++) {
        char *path = g_build_filename(directory, names[name], NULL);
        g_strv_builder_add(builder, path);
        g_free(path);
      }
      continue;
    }
    char **parts = g_strsplit(argv[i], "%dir", -1);
    char *word = g_strjoinv(directory, parts);
    g_strfreev(parts);
    g_strv_builder_add(builder, word);
    g_free(word);
  }
  char **expanded = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  return expanded;
}
