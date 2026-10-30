#ifndef LEME_DESK_THEME_H
#define LEME_DESK_THEME_H

#include <stdbool.h>

bool leme_desk_theme_resolve(const char *theme, const char *name,
                             int logical_size, int scale, char **path);
bool leme_desk_theme_installed(const char *theme);

#endif
