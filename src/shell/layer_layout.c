#include "shell/layer_layout.h"

#include "shell/layer.h"
#include "shell/scratchpad.h"
#include "output/output.h"
#include "render/render.h"
#include "core/server.h"
#include "shell/view.h"
#include "shell/xwayland.h"

#include <stdint.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

static bool
leme_layer_box_equal(struct leme_box first, struct leme_box second)
{
    return first.x == second.x && first.y == second.y &&
        first.width == second.width && first.height == second.height;
}

static void
leme_layer_arrange_priority(struct leme_server *server,
    const struct leme_output *output, uint32_t priority,
    bool exclusive, struct leme_box full, struct leme_box *usable)
{
    struct leme_layer_surface *layer;

    wl_list_for_each(layer, &server->layer_surfaces, link) {
        struct wlr_layer_surface_v1 *surface = layer->wlr_layer_surface;

        if (layer->output != output ||
                layer->scene_layer_surface == NULL ||
                (!layer->mapped && surface->configured) ||
                surface->current.layer != priority ||
                (surface->current.exclusive_zone > 0) != exclusive) {
            continue;
        }
        leme_render_layer_configure(layer, full, usable);
    }
}

void
leme_layer_arrange(struct leme_server *server)
{
    static const uint32_t priorities[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    struct leme_output *output;
    bool changed = false;

    if (server->outputs.next == NULL) {
        leme_xwayland_update_workarea(server);
        return;
    }
    wl_list_for_each(output, &server->outputs, link) {
        struct leme_box full;
        struct leme_box usable;
        struct leme_box previous;
        size_t index;

        if (!output->wlr_output->enabled) {
            continue;
        }
        full = leme_output_full_box(output);
        usable = full;
        previous = output->usable_box;

        for (index = 0; index < LEME_ARRAY_LENGTH(priorities); index++) {
            leme_layer_arrange_priority(server, output, priorities[index],
                true, full, &usable);
        }
        for (index = 0; index < LEME_ARRAY_LENGTH(priorities); index++) {
            leme_layer_arrange_priority(server, output, priorities[index],
                false, full, &usable);
        }

        output->usable_box = usable;
        /* As fracções nomeadas são política de exibição. Nesta costura
         * final da área útil, preserva-se a caixa actual e só se limita. */
        leme_scratchpad_handle_usable_area(output);
        if (!leme_layer_box_equal(previous, usable)) {
            wlr_log(WLR_INFO, "leme: usable area on %s %dx%d+%d+%d",
                output->wlr_output->name, usable.width, usable.height,
                usable.x, usable.y);
            changed = true;
        }
    }
    if (changed) {
        leme_view_arrange(server);
    }
    leme_xwayland_update_workarea(server);
}
