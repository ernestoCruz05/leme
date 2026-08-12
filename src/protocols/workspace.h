#ifndef LEME_WORKSPACE_H
#define LEME_WORKSPACE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct leme_output;
struct leme_server;

bool leme_workspace_init(struct leme_server *server);
void leme_workspace_finish(struct leme_server *server);
void leme_workspace_reconcile(struct leme_server *server);
void leme_workspace_release_output(struct leme_output *output);
void leme_workspace_handle_requests(struct leme_server *server,
    struct wl_list *requests);
void leme_workspace_mark_urgent(struct leme_server *server,
    struct leme_output *output, uint16_t tag_id);

#endif
