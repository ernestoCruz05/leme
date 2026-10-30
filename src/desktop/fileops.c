#define _GNU_SOURCE

#include "desktop/fileops.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool leme_desk_file_component(const char *name) {
  return name != NULL && name[0] != '\0' && strchr(name, '/') == NULL &&
         strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

bool leme_desk_file_name_valid(const char *name) {
  if (name == NULL || strnlen(name, 1025) > 1024 ||
      !g_utf8_validate(name, -1, NULL) || !leme_desk_file_component(name)) {
    return false;
  }
  for (const char *p = name; *p != '\0'; p = g_utf8_next_char(p)) {
    if (g_unichar_iscntrl(g_utf8_get_char(p))) {
      return false;
    }
  }
  return true;
}

static void leme_desk_file_error(struct leme_desk_file_result *result,
                                 const char *message, const char *detail) {
  result->success = false;
  const int length =
      snprintf(result->message, sizeof(result->message), "%s%s%s", message,
               detail != NULL ? ": " : "", detail != NULL ? detail : "");
  if (length < 0) {
    result->message[0] = '\0';
  } else if ((size_t)length >= sizeof(result->message)) {
    result->message[sizeof(result->message) - 1] = '\0';
  }
  const char *end = NULL;
  if (!g_utf8_validate(result->message, -1, &end)) {
    result->message[end - result->message] = '\0';
  }
  if (result->message[0] == '\0') {
    (void)g_strlcpy(result->message, "File operation failed",
                    sizeof(result->message));
  }
}

bool leme_desk_file_run(const struct leme_desk_file_request *request,
                        struct leme_desk_file_result *result) {
  *result = (struct leme_desk_file_result){0};
  const bool typed = request->action == LEME_DESK_FILE_CREATE_FOLDER ||
                     request->action == LEME_DESK_FILE_RENAME;
  if (request->directory == NULL || request->directory[0] != '/' ||
      (request->action != LEME_DESK_FILE_CREATE_FOLDER &&
       !leme_desk_file_component(request->basename)) ||
      (typed && !leme_desk_file_name_valid(request->name)) ||
      (request->action == LEME_DESK_FILE_MOVE_INTO &&
       !leme_desk_file_component(request->name))) {
    leme_desk_file_error(result, "Invalid filename", NULL);
    return false;
  }
  const int directory =
      open(request->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) {
    leme_desk_file_error(result, "Cannot open desktop folder",
                         g_strerror(errno));
    return false;
  }
  char *name = NULL;
  GError *error = NULL;
  if (typed) {
    name = g_filename_from_utf8(request->name, -1, NULL, NULL, &error);
    if (name == NULL) {
      leme_desk_file_error(result, "Cannot encode name", error->message);
      goto cleanup;
    }
    if (!leme_desk_file_component(name)) {
      leme_desk_file_error(result, "Invalid filename", NULL);
      goto cleanup;
    }
  }
  switch (request->action) {
  case LEME_DESK_FILE_CREATE_FOLDER:
    if (mkdirat(directory, name, 0777) < 0) {
      leme_desk_file_error(result, "Cannot create folder", g_strerror(errno));
    } else {
      result->success = true;
    }
    break;
  case LEME_DESK_FILE_RENAME:
    if (strcmp(request->basename, name) == 0 ||
        renameat2(directory, request->basename, directory, name,
                  RENAME_NOREPLACE) == 0) {
      result->success = true;
    } else {
      leme_desk_file_error(result, "Cannot rename entry", g_strerror(errno));
    }
    break;
  case LEME_DESK_FILE_MOVE_INTO: {
    const int folder =
        openat(directory, request->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder < 0) {
      leme_desk_file_error(result, "Cannot open folder", g_strerror(errno));
      break;
    }
    if (renameat2(directory, request->basename, folder, request->basename,
                  RENAME_NOREPLACE) == 0) {
      result->success = true;
    } else {
      leme_desk_file_error(result, "Cannot move entry", g_strerror(errno));
    }
    (void)close(folder);
    break;
  }
  case LEME_DESK_FILE_TRASH: {
    char *path = g_build_filename(request->directory, request->basename, NULL);
    GFile *file = g_file_new_for_path(path);
    result->success = g_file_trash(file, NULL, &error);
    if (!result->success) {
      leme_desk_file_error(result, "Cannot move entry to Trash",
                           error->message);
    }
    g_object_unref(file);
    g_free(path);
    break;
  }
  }
cleanup:
  g_clear_error(&error);
  g_free(name);
  (void)close(directory);
  return result->success;
}
