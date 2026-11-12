#ifndef LEME_DESK_WATCH_H
#define LEME_DESK_WATCH_H

#include <stdbool.h>
#include <stdint.h>

struct wl_event_loop;
struct leme_desk_watch;

struct leme_desk_watch *leme_desk_watch_create(struct wl_event_loop *loop,
                                               const char *directory,
                                               void (*changed)(void *data),
                                               void *data);
void leme_desk_watch_destroy(struct leme_desk_watch *watch);
void leme_desk_watch_begin_files(struct leme_desk_watch *watch);
bool leme_desk_watch_add_file(struct leme_desk_watch *watch, const char *path);
void leme_desk_watch_end_files(struct leme_desk_watch *watch);

#endif
