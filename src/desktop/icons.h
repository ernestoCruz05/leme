#ifndef LEME_DESK_ICONS_H
#define LEME_DESK_ICONS_H

struct leme_desk_icons;
struct wlr_buffer;

struct leme_desk_icons *leme_desk_icons_create(void);
void leme_desk_icons_destroy(struct leme_desk_icons *icons);
void leme_desk_icons_clear(struct leme_desk_icons *icons);
void leme_desk_icons_prune(struct leme_desk_icons *icons);
void leme_desk_icons_for_each_path(const struct leme_desk_icons *icons,
                                   void (*visit)(const char *path, void *data),
                                   void *data);
struct wlr_buffer *leme_desk_icons_get(struct leme_desk_icons *icons,
                                       const char *path, int pixels);

#endif
