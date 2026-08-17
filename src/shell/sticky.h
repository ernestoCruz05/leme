#ifndef LEME_STICKY_H
#define LEME_STICKY_H

#include "core/leme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct leme_output;
struct leme_server;
struct leme_sticky_adoption;
struct leme_sticky_output_plan;

struct leme_sticky_output_snapshot {
    struct leme_output *output;
    struct leme_box future_full;
    bool usable;
};

enum leme_sticky_output_prepare_checkpoint {
    LEME_STICKY_OUTPUT_PREPARE_PLAN,
    LEME_STICKY_OUTPUT_PREPARE_ENTRY,
};

enum leme_sticky_unmap_prepare_checkpoint {
    LEME_STICKY_UNMAP_PREPARE_VIEWS,
    LEME_STICKY_UNMAP_PREPARE_OWNER,
};
struct leme_view;
struct wlr_scene_tree;

struct leme_sticky_manager {
    struct leme_server *server;
    struct wl_list groups;
};

struct leme_sticky_member;

bool leme_sticky_init(struct leme_server *server);
void leme_sticky_finish(struct leme_server *server);
bool leme_view_is_sticky(const struct leme_view *view);
bool leme_sticky_is_dependent(const struct leme_view *view);
struct leme_view *leme_sticky_group_root(struct leme_view *view);
struct wlr_scene_tree *leme_sticky_render_parent(
    const struct leme_view *view);
bool leme_sticky_toggle(struct leme_view *view);
bool leme_sticky_prepare_transient(struct leme_view *parent,
    struct leme_view *dependent,
    struct leme_sticky_adoption **adoption);
void leme_sticky_commit_transient(
    struct leme_sticky_adoption **adoption);
void leme_sticky_discard_transient(
    struct leme_sticky_adoption **adoption);
bool leme_sticky_apply_box(struct leme_view *view,
    struct leme_box box, bool resizing);
bool leme_sticky_move_to_output(struct leme_view *view,
    struct leme_output *output, bool follow);
bool leme_sticky_move_to_tag(struct leme_view *view,
    uint16_t tag_id, bool follow);
bool leme_sticky_enter_fullscreen(struct leme_view *view);
bool leme_sticky_prepare_outputs(struct leme_server *server,
    const struct leme_sticky_output_snapshot *snapshots,
    size_t snapshot_count,
    struct leme_sticky_output_plan **plan);
void leme_sticky_commit_outputs(struct leme_sticky_output_plan **plan);
void leme_sticky_discard_outputs(struct leme_sticky_output_plan **plan);
void leme_sticky_handle_output_destroy(struct leme_server *server,
    struct leme_output *output, struct leme_output *successor);
void leme_sticky_handle_usable_area(struct leme_output *output);
void leme_sticky_raise_group(struct leme_view *view);
struct leme_view *leme_sticky_focus_candidate(struct leme_server *server,
    struct leme_output *output);
void leme_sticky_handle_unmap(struct leme_view *view);

#endif
