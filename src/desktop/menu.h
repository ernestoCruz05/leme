#ifndef LEME_DESK_MENU_H
#define LEME_DESK_MENU_H

#include "core/leme.h"
#include "desktop/target.h"

#define LEME_DESK_MENU_ITEMS_MAX 64

enum leme_desk_menu_action {
  LEME_DESK_MENU_NONE,
  LEME_DESK_MENU_MINIMIZE,
  LEME_DESK_MENU_MAXIMIZE,
  LEME_DESK_MENU_FULLSCREEN,
  LEME_DESK_MENU_STICKY,
  LEME_DESK_MENU_TAG,
  LEME_DESK_MENU_OUTPUT,
  LEME_DESK_MENU_LAYOUT,
  LEME_DESK_MENU_DETACH,
  LEME_DESK_MENU_DISSOLVE,
  LEME_DESK_MENU_CLOSE,
  LEME_DESK_MENU_SUBMENU,
  LEME_DESK_MENU_COMMAND,
};

enum leme_desk_menu_list {
  LEME_DESK_MENU_MAIN,
  LEME_DESK_MENU_TAGS,
  LEME_DESK_MENU_OUTPUTS,
  LEME_DESK_MENU_LAYOUTS,
  LEME_DESK_MENU_DESKTOP,
  LEME_DESK_MENU_ENTRY,
};

struct leme_desk_menu_item {
  char label[64];
  enum leme_desk_menu_action action;
  enum leme_desk_menu_list submenu;
  uint64_t value;
  bool enabled;
  bool checked;
  bool separator;
};

struct leme_server;
struct leme_box leme_desk_menu_place(struct leme_box area, double x, double y,
                                     int width, int height);

bool leme_desk_menu_open(struct leme_server *server,
                         struct leme_desk_target target, double x, double y);
struct leme_output;
bool leme_desk_menu_open_desktop(struct leme_server *server,
                                 struct leme_output *output, bool entry,
                                 double x, double y);
void leme_desk_menu_close(struct leme_server *server);
void leme_desk_menu_finish(struct leme_server *server);
void leme_desk_menu_invalidate(struct leme_server *server,
                               struct leme_desk_target target);

#endif
