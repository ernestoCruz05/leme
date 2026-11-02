#include "desktop/app-icon.h"

#include "desktop/catalog.h"
#include "desktop/grid.h"
#include "desktop/theme.h"

#include <glib.h>
#include <string.h>
#include <sys/stat.h>

static bool leme_desk_app_icon_identity(const char *identity) {
  return identity != NULL && identity[0] != '\0' &&
         strnlen(identity, 256) < 256 && strchr(identity, '/') == NULL &&
         strcmp(identity, ".") != 0 && strcmp(identity, "..") != 0 &&
         g_utf8_validate(identity, -1, NULL);
}

static char *leme_desk_app_icon_read(const char *root, const char *filename,
                                     bool *found) {
  if (root == NULL || root[0] != '/') {
    return NULL;
  }
  char *path = g_build_filename(root, "applications", filename, NULL);
  if (path == NULL) {
    return NULL;
  }
  struct stat status = {0};
  char *icon = NULL;
  if (stat(path, &status) == 0) {
    *found = true;
    if (S_ISREG(status.st_mode)) {
      icon = leme_desk_catalog_launcher_icon(path);
    }
  }
  g_free(path);
  return icon;
}

static char *leme_desk_app_icon_metadata(const char *identity) {
  char *filename = g_strconcat(identity, ".desktop", NULL);
  if (filename == NULL) {
    return NULL;
  }
  bool found = false;
  char *icon = leme_desk_app_icon_read(g_get_user_data_dir(), filename, &found);
  const char *const *roots = g_get_system_data_dirs();
  for (size_t i = 0; !found && roots[i] != NULL; i++) {
    icon = leme_desk_app_icon_read(roots[i], filename, &found);
  }
  g_free(filename);
  return icon;
}

static bool leme_desk_app_icon_named(const char *name, const char *theme,
                                     int scale, char **path) {
  return name != NULL && leme_desk_theme_resolve(
                             theme, name, LEME_DESK_STRIP_ICON, scale, path);
}

static bool leme_desk_app_icon_lookup(const char *app_id, const char *theme,
                                      int scale, char **path) {
  if (path == NULL) {
    return false;
  }
  *path = NULL;
  if (scale < 1 || scale > 16) {
    return false;
  }
  if (leme_desk_app_icon_identity(app_id)) {
    const size_t length = strlen(app_id);
    char *identity = g_strndup(
        app_id, g_str_has_suffix(app_id, ".desktop") ? length - 8 : length);
    if (identity != NULL && leme_desk_app_icon_identity(identity)) {
      char *lower = g_ascii_strdown(identity, -1);
      char *icon = leme_desk_app_icon_metadata(identity);
      bool found = leme_desk_app_icon_named(icon, theme, scale, path);
      g_free(icon);
      if (!found && lower != NULL && strcmp(identity, lower) != 0) {
        icon = leme_desk_app_icon_metadata(lower);
        found = leme_desk_app_icon_named(icon, theme, scale, path);
        g_free(icon);
      }
      if (!found) {
        found = leme_desk_app_icon_named(identity, theme, scale, path) ||
                leme_desk_app_icon_named(lower, theme, scale, path);
      }
      g_free(lower);
      g_free(identity);
      if (found) {
        return true;
      }
    } else {
      g_free(identity);
    }
  }
  return leme_desk_app_icon_named("application-x-executable", theme, scale,
                                  path);
}

bool leme_desk_app_icon_resolve(const char *app_id, const char *theme,
                                int scale, char **path) {
  return leme_desk_app_icon_lookup(app_id, theme, scale, path) ||
         (theme != NULL && theme[0] == '/' &&
          leme_desk_app_icon_lookup(app_id, LEME_DESK_THEME_DEFAULT, scale,
                                    path));
}
