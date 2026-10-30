#ifndef LEME_DESKTOP_FILEOPS_H
#define LEME_DESKTOP_FILEOPS_H

#include <stdbool.h>

enum leme_desk_file_action {
  LEME_DESK_FILE_CREATE_FOLDER,
  LEME_DESK_FILE_RENAME,
  LEME_DESK_FILE_TRASH,
  LEME_DESK_FILE_MOVE_INTO,
};

struct leme_desk_file_request {
  enum leme_desk_file_action action;
  const char *directory;
  const char *basename;
  const char *name;
};

struct leme_desk_file_result {
  bool success;
  char message[512];
};

bool leme_desk_file_name_valid(const char *name);
bool leme_desk_file_run(const struct leme_desk_file_request *request,
                        struct leme_desk_file_result *result);

#endif
