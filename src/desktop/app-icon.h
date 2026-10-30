#ifndef LEME_DESK_APP_ICON_H
#define LEME_DESK_APP_ICON_H

#include <stdbool.h>

bool leme_desk_app_icon_resolve(const char *app_id, const char *theme,
                                int scale, char **path);

#endif
