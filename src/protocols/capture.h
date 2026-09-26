#ifndef LEME_CAPTURE_H
#define LEME_CAPTURE_H

#include <stdbool.h>

struct leme_server;
struct leme_view;
struct leme_view_popup;

bool leme_capture_view_eligible(const struct leme_server *server,
                                const struct leme_view *view);
void leme_capture_invalidate_view(struct leme_view *view);
void leme_capture_invalidate_all(struct leme_server *server);
void leme_capture_sync(struct leme_server *server);
void leme_capture_popup_create(struct leme_view_popup *popup);
void leme_capture_popup_destroy(struct leme_view_popup *popup);
bool leme_capture_init(struct leme_server *server);
void leme_capture_finish(struct leme_server *server);

#endif
