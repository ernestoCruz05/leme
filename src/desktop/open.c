#define _XOPEN_SOURCE 700

#include "desktop/open.h"

#include "config/config.h"
#include "core/process.h"
#include "core/server.h"

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
                                          const char *resolved, bool launchers,
                                          bool *launch) {
  struct stat status = {0};
  *launch = false;
  if (stat(resolved, &status) != 0) {
    return "cannot inspect target";
  }
  if (S_ISDIR(status.st_mode)) {
    return NULL;
  }
  if (!S_ISREG(status.st_mode)) {
    return "special files cannot be opened from the desktop";
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
  const bool launcher = leme_desk_open_launcher(path, NULL, 0) ||
                        leme_desk_open_launcher(resolved, NULL, 0) ||
                        leme_desk_open_launcher(NULL, data, (size_t)used);
  if (!launcher) {
    return (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0
               ? "executable files are disabled"
               : NULL;
  }
  if (!launchers) {
    return "desktop launchers are disabled";
  }
  if (status.st_uid != geteuid()) {
    return "launchers must belong to you";
  }
  if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return "launchers must not be writable by others";
  }
  if ((status.st_mode & S_IXUSR) == 0) {
    return "launchers must be executable; mark it with chmod +x";
  }
  *launch = true;
  return NULL;
}

static bool leme_desk_open_field_code(const char *word) {
  static const char *const dropped[] = {"%f", "%F", "%u", "%U", "%d",
                                        "%D", "%n", "%N", "%v", "%m"};
  for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++) {
    if (strcmp(word, dropped[i]) == 0) {
      return true;
    }
  }
  return false;
}

static char *leme_desk_open_expand_word(const char *word, const char *name,
                                        const char *location) {
  GString *out = g_string_new(NULL);
  for (const char *c = word; *c != '\0'; c++) {
    if (c[0] != '%' || c[1] == '\0') {
      g_string_append_c(out, *c);
      continue;
    }
    c++;
    if (*c == '%') {
      g_string_append_c(out, '%');
    } else if (*c == 'c') {
      g_string_append(out, name);
    } else if (*c == 'k') {
      g_string_append(out, location);
    }
  }
  return g_string_free(out, false);
}

static char **leme_desk_open_launcher_argv(const char *exec, const char *name,
                                           const char *icon,
                                           const char *location) {
  int count = 0;
  char **words = NULL;
  if (exec == NULL || !g_shell_parse_argv(exec, &count, &words, NULL)) {
    return NULL;
  }
  GStrvBuilder *builder = g_strv_builder_new();
  for (int i = 0; i < count; i++) {
    if (leme_desk_open_field_code(words[i])) {
      continue;
    }
    if (strcmp(words[i], "%i") == 0) {
      if (icon != NULL && icon[0] != '\0') {
        g_strv_builder_add(builder, "--icon");
        g_strv_builder_add(builder, icon);
      }
      continue;
    }
    char *word = leme_desk_open_expand_word(words[i], name == NULL ? "" : name,
                                            location);
    g_strv_builder_take(builder, word);
  }
  g_strfreev(words);
  char **argv = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  if (argv[0] == NULL || argv[0][0] == '\0') {
    g_strfreev(argv);
    return NULL;
  }
  return argv;
}

static const char *leme_desk_open_launch(struct leme_server *server,
                                         const char *resolved) {
  GKeyFile *key = g_key_file_new();
  const char *reason = NULL;
  char **argv = NULL;
  if (!g_key_file_load_from_file(key, resolved, G_KEY_FILE_NONE, NULL)) {
    reason = "cannot read the launcher";
    goto out;
  }
  char *type = g_key_file_get_string(key, "Desktop Entry", "Type", NULL);
  if (type != NULL && strcmp(type, "Link") == 0) {
    char *url = g_key_file_get_string(key, "Desktop Entry", "URL", NULL);
    if (url != NULL && url[0] != '\0') {
      argv = g_new0(char *, 3);
      argv[0] = g_strdup("xdg-open");
      argv[1] = url;
    } else {
      g_free(url);
      reason = "the link has no URL";
    }
  } else if (type != NULL && strcmp(type, "Application") == 0) {
    if (g_key_file_get_boolean(key, "Desktop Entry", "Terminal", NULL)) {
      reason = "terminal launchers are not supported";
    } else {
      char *exec = g_key_file_get_string(key, "Desktop Entry", "Exec", NULL);
      char *name = g_key_file_get_locale_string(key, "Desktop Entry", "Name",
                                                NULL, NULL);
      char *icon = g_key_file_get_string(key, "Desktop Entry", "Icon", NULL);
      argv = leme_desk_open_launcher_argv(exec, name, icon, resolved);
      if (argv == NULL) {
        reason = "the launcher has no command to run";
      }
      g_free(exec);
      g_free(name);
      g_free(icon);
    }
  } else {
    reason = "only Application and Link launchers can be opened";
  }
  g_free(type);
  if (argv != NULL && !leme_process_spawn_detached(server, argv)) {
    reason = "cannot start the launcher's command";
  }
out:
  g_strfreev(argv);
  g_key_file_unref(key);
  return reason;
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
  bool launch = false;
  const bool launchers =
      server->config == NULL || server->config->desktop_launchers;
  const char *reason =
      leme_desk_open_refusal(path, resolved, launchers, &launch);
  bool opened = false;
  if (reason == NULL && launch) {
    reason = leme_desk_open_launch(server, resolved);
    opened = reason == NULL;
  } else if (reason == NULL) {
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
