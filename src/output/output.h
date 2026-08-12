#ifndef LEME_OUTPUT_H
#define LEME_OUTPUT_H

#include "core/leme.h"
#include "workspace/layout.h"
#include "workspace/tag.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

struct leme_config;
struct leme_server;
struct leme_tags;
struct leme_view;
struct leme_workspace_transition;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_scene_output;

struct leme_output {
    struct leme_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct leme_tags tags;
    struct leme_workspace_transition *workspace_transition;
    struct leme_box full_box;
    struct leme_box usable_box;
    int layout_x;
    int layout_y;
    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wlr_ext_workspace_group_handle_v1 *workspace_group;
    bool power_on;
    bool tearing_fallback;
};

bool leme_output_init(struct leme_server *server);
void leme_output_finish(struct leme_server *server);
bool leme_output_test_config(struct leme_server *server,
    const struct leme_config *config);
bool leme_output_apply_config(struct leme_server *server,
    const struct leme_config *config, bool startup);
bool leme_output_set_power(struct leme_output *output, bool on);
void leme_output_publish_configuration(struct leme_server *server);
struct leme_box leme_output_usable_box(const struct leme_output *output);
struct leme_box leme_output_full_box(const struct leme_output *output);
void leme_output_refresh_geometry(struct leme_output *output);
struct leme_output *leme_output_focused(const struct leme_server *server);
struct leme_output *leme_output_from_wlr_output(
    struct leme_server *server, const struct wlr_output *wlr_output);
struct leme_output *leme_output_adjacent(struct leme_server *server,
    const struct leme_output *origin, enum leme_direction direction);
struct leme_output *leme_output_by_name(
    struct leme_server *server, const char *name);
struct leme_output *leme_output_at(
    struct leme_server *server, double layout_x, double layout_y);
void leme_output_set_focused(struct leme_server *server,
    struct leme_output *output, bool warp);
struct leme_output *leme_view_output(const struct leme_view *view);
struct leme_tags *leme_output_tags(struct leme_output *output);
struct leme_tags *leme_focused_tags(const struct leme_server *server);

#endif
