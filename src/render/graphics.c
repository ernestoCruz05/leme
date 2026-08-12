#include "render/graphics.h"

#include "core/server.h"

#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/util/log.h>

bool
leme_graphics_init(struct leme_server *server)
{
    const struct wlr_drm_format_set *dmabuf_formats =
        wlr_renderer_get_texture_formats(
            server->renderer, WLR_BUFFER_CAP_DMABUF);
    int drm_fd = wlr_renderer_get_drm_fd(server->renderer);

    if (dmabuf_formats != NULL && drm_fd >= 0) {
        server->linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(
            server->display, 5, server->renderer);
        if (server->linux_dmabuf == NULL) {
            goto error;
        }
        wlr_scene_set_linux_dmabuf_v1(
            server->scene, server->linux_dmabuf);
    } else {
        wlr_log(WLR_INFO, "%s",
            "leme: renderer does not support linux-dmabuf");
    }

    if (drm_fd >= 0 && server->renderer->features.timeline &&
            server->backend->features.timeline) {
        server->linux_drm_syncobj =
            wlr_linux_drm_syncobj_manager_v1_create(
                server->display, 1, drm_fd);
        if (server->linux_drm_syncobj == NULL) {
            goto error;
        }
    } else {
        wlr_log(WLR_INFO, "%s",
            "leme: renderer/backend does not support explicit sync");
    }

    server->presentation =
        wlr_presentation_create(server->display, server->backend, 2);
    server->single_pixel_buffer_manager =
        wlr_single_pixel_buffer_manager_v1_create(server->display);
    server->alpha_modifier = wlr_alpha_modifier_v1_create(server->display);
    server->content_type_manager =
        wlr_content_type_manager_v1_create(server->display, 1);
    if (server->presentation == NULL ||
            server->single_pixel_buffer_manager == NULL ||
            server->alpha_modifier == NULL ||
            server->content_type_manager == NULL) {
        goto error;
    }
    return true;

error:
    wlr_log(WLR_ERROR, "%s", "leme: failed to create graphics globals");
    return false;
}

void
leme_graphics_finish(struct leme_server *server)
{
    server->linux_dmabuf = NULL;
    server->linux_drm_syncobj = NULL;
    server->presentation = NULL;
    server->single_pixel_buffer_manager = NULL;
    server->alpha_modifier = NULL;
    server->content_type_manager = NULL;
}
