#include "protocols/tearing.h"

#include "output/output.h"
#include "core/server.h"
#include "protocols/session.h"
#include "workspace/tag.h"
#include "shell/view.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>

struct leme_tearing {
    struct wlr_tearing_control_manager_v1 *manager;
};

bool
leme_tearing_init(struct leme_server *server)
{
    struct leme_tearing *tearing = calloc(1, sizeof(*tearing));

    if (tearing == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create tearing control protocol");
        return false;
    }
    tearing->manager =
        wlr_tearing_control_manager_v1_create(server->display, 1);
    if (tearing->manager == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create tearing control protocol");
        free(tearing);
        return false;
    }
    server->tearing = tearing;
    return true;
}

void
leme_tearing_finish(struct leme_server *server)
{
    free(server->tearing);
    server->tearing = NULL;
}

bool
leme_tearing_can_tear(const struct leme_server *server,
    struct leme_output *output)
{
    struct leme_tags *tags = leme_output_tags(output);
    struct leme_view *view;
    struct wlr_surface *surface;

    if (server == NULL || server->tearing == NULL ||
            output == NULL || output->scene_output == NULL ||
            !output->wlr_output->enabled ||
            leme_session_locked(server) || server->focused_view == NULL) {
        return false;
    }
    view = server->focused_view;
    if (!view->mapped || !view->fullscreen || view->tag == NULL ||
            tags == NULL || tags->focused_is_candidate ||
            view->tag->id != tags->focused_id ||
            leme_view_output(view) != output) {
        return false;
    }
    surface = leme_view_surface(view);
    if (surface == NULL) {
        return false;
    }
    surface = wlr_surface_get_root_surface(surface);
    return wlr_tearing_control_manager_v1_surface_hint_from_surface(
        server->tearing->manager, surface) ==
        WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}
