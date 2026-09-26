#ifndef LEME_PUBLIC_SERVER_H
#define LEME_PUBLIC_SERVER_H

#include "public/model.h"

struct leme_server;
struct leme_view;

struct leme_public_features leme_public_server_features(const struct leme_server *server);
bool leme_public_server_init(struct leme_server *server);
bool leme_public_server_prepare(struct leme_server *server);
void leme_public_server_finish(struct leme_server *server);
void leme_public_server_invalidate(struct leme_server *server);
void leme_public_server_config_changed(struct leme_server *server);
void leme_public_server_lock_changed(struct leme_server *server, bool locked);
void leme_public_server_view_mapped(struct leme_view *view);
void leme_public_server_view_focused(struct leme_view *view, bool changed);
void leme_public_server_view_urgent(struct leme_view *view);
enum leme_public_status
leme_public_server_capture(struct leme_server *server, uint32_t requested_roots,
                           struct leme_public_snapshot **out);
enum leme_public_status
leme_public_server_capture_work(struct leme_server *server, uint32_t roots,
                                const struct leme_public_work *work,
                                struct leme_public_snapshot **out);
struct leme_public_source leme_public_server_source(struct leme_server *server);

#endif
