#ifndef LEME_TOPLEVEL_H
#define LEME_TOPLEVEL_H

#include <stdbool.h>

struct leme_server;
struct leme_view;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_output;

bool leme_toplevel_init(struct leme_server *server);
void leme_toplevel_finish(struct leme_server *server);
void leme_toplevel_reconcile(struct leme_server *server);
void leme_toplevel_untrack(struct leme_view *view);
void leme_toplevel_activate_view(struct leme_view *view);
struct leme_view *leme_toplevel_view_from_handle(
    struct wlr_ext_foreign_toplevel_handle_v1 *handle);
/* NULL enquanto a vista não estiver publicada. */
struct wlr_foreign_toplevel_handle_v1 *leme_toplevel_handle(
    const struct leme_view *view);

#endif
