#include "protocols/publication.h"

#include "core/server.h"
#include "protocols/session.h"
#include "protocols/toplevel.h"
#include "ipc/ipc.h"
#include "protocols/workspace.h"

#include <wlr/util/log.h>

static void
leme_publication_reconcile(struct leme_server *server)
{
    server->publication_dirty = false;
    if (leme_session_locked(server)) {
        return;
    }
    leme_workspace_reconcile(server);
    leme_toplevel_reconcile(server);
}

static void
leme_publication_handle_idle(void *data)
{
    struct leme_server *server = data;

    server->publication_idle = NULL;
    leme_publication_reconcile(server);
}

void
leme_publication_invalidate(struct leme_server *server)
{
    struct wl_event_loop *loop;

    if (server == NULL || server->display == NULL) {
        return;
    }
    leme_ipc_invalidate(server);
    if (server->workspace_manager == NULL) {
        return;
    }
    server->publication_dirty = true;
    if (server->publication_idle != NULL) {
        return;
    }
    loop = wl_display_get_event_loop(server->display);
    server->publication_idle = wl_event_loop_add_idle(loop,
        leme_publication_handle_idle, server);
    if (server->publication_idle == NULL) {
        leme_publication_reconcile(server);
    }
}

bool
leme_publication_init(struct leme_server *server)
{
    if (!leme_workspace_init(server)) {
        goto error;
    }
    if (!leme_toplevel_init(server)) {
        goto error;
    }
    return true;

error:
    wlr_log(WLR_ERROR, "%s", "leme: failed to create publication globals");
    leme_toplevel_finish(server);
    leme_workspace_finish(server);
    return false;
}

void
leme_publication_finish(struct leme_server *server)
{
    if (server->publication_idle != NULL) {
        wl_event_source_remove(server->publication_idle);
        server->publication_idle = NULL;
    }
    server->publication_dirty = false;
    leme_toplevel_finish(server);
    leme_workspace_finish(server);
}
