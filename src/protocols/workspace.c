#include "protocols/workspace.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/publication.h"
#include "protocols/session.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#define LEME_WORKSPACE_MAX_IDS 64

struct leme_workspace_entry {
    struct wlr_ext_workspace_handle_v1 *handle;
    struct leme_output *output;
    uint16_t tag_id;
    bool urgent;
    bool seen;
    struct wl_list link;
};

static struct leme_workspace_entry *
leme_workspace_find(struct leme_server *server,
    const struct leme_output *output, uint16_t tag_id)
{
    struct leme_workspace_entry *entry;

    wl_list_for_each(entry, &server->workspace_entries, link) {
        if (entry->output == output && entry->tag_id == tag_id) {
            return entry;
        }
    }
    return NULL;
}

static void
leme_workspace_entry_destroy(struct leme_workspace_entry *entry)
{
    wl_list_remove(&entry->link);
    if (entry->handle != NULL) {
        wlr_ext_workspace_handle_v1_destroy(entry->handle);
    }
    free(entry);
}

static struct leme_workspace_entry *
leme_workspace_entry_create(struct leme_server *server,
    struct leme_output *output, uint16_t tag_id)
{
    struct leme_workspace_entry *entry = calloc(1, sizeof(*entry));
    char id[128] = {0};
    char name[8] = {0};
    int written;

    if (entry == NULL) {
        return NULL;
    }
    written = snprintf(id, sizeof(id), "%s:%u",
        output->wlr_output->name, tag_id);
    if (written < 0 || (size_t)written >= sizeof(id)) {
        free(entry);
        return NULL;
    }
    written = snprintf(name, sizeof(name), "%u", tag_id);
    if (written < 0 || (size_t)written >= sizeof(name)) {
        free(entry);
        return NULL;
    }
    entry->handle = wlr_ext_workspace_handle_v1_create(
        server->workspace_manager, id,
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
    if (entry->handle == NULL) {
        free(entry);
        return NULL;
    }
    entry->output = output;
    entry->tag_id = tag_id;
    entry->handle->data = entry;
    wlr_ext_workspace_handle_v1_set_name(entry->handle, name);
    wlr_ext_workspace_handle_v1_set_group(
        entry->handle, output->workspace_group);
    wl_list_insert(&server->workspace_entries, &entry->link);
    return entry;
}

static bool
leme_workspace_ensure_group(struct leme_output *output)
{
    if (output->workspace_group != NULL) {
        return true;
    }
    output->workspace_group = wlr_ext_workspace_group_handle_v1_create(
        output->server->workspace_manager, 0);
    if (output->workspace_group == NULL) {
        return false;
    }
    output->workspace_group->data = output;
    wlr_ext_workspace_group_handle_v1_output_enter(
        output->workspace_group, output->wlr_output);
    return true;
}

static void
leme_workspace_sync_output(struct leme_server *server,
    struct leme_output *output)
{
    struct leme_tags *tags = leme_output_tags(output);
    uint16_t ids[LEME_WORKSPACE_MAX_IDS] = {0};
    size_t count;
    size_t index;

    if (tags == NULL) {
        return;
    }
    if (!leme_workspace_ensure_group(output)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create workspace group");
        return;
    }
    count = leme_tags_navigable(tags, ids, LEME_WORKSPACE_MAX_IDS);
    if (count > LEME_WORKSPACE_MAX_IDS) {
        count = LEME_WORKSPACE_MAX_IDS;
    }
    for (index = 0; index < count; index++) {
        struct leme_workspace_entry *entry =
            leme_workspace_find(server, output, ids[index]);
        uint32_t coordinates = ids[index];
        bool active;

        if (entry == NULL) {
            entry = leme_workspace_entry_create(server, output, ids[index]);
            if (entry == NULL) {
                continue;
            }
        }
        entry->seen = true;
        active = tags->focused_id == ids[index];
        if (active) {
            entry->urgent = false;
        }
        wlr_ext_workspace_handle_v1_set_coordinates(
            entry->handle, &coordinates, 1);
        wlr_ext_workspace_handle_v1_set_active(entry->handle, active);
        wlr_ext_workspace_handle_v1_set_urgent(entry->handle, entry->urgent);
    }
}

void
leme_workspace_mark_urgent(struct leme_server *server,
    struct leme_output *output, uint16_t tag_id)
{
    struct leme_workspace_entry *entry =
        leme_workspace_find(server, output, tag_id);

    if (entry != NULL) {
        entry->urgent = true;
    }
}

static void
leme_workspace_activate(struct leme_server *server,
    struct leme_workspace_entry *entry)
{
    enum leme_activation_policy policy = LEME_ACTIVATION_FOLLOW;
    struct leme_tags *tags;

    if (entry == NULL || entry->output == NULL) {
        return;
    }
    if (server->config != NULL) {
        policy = server->config->publication.activation;
    }
    tags = leme_output_tags(entry->output);
    if (tags == NULL) {
        return;
    }
    if (entry->output != leme_output_focused(server)) {
        if (policy == LEME_ACTIVATION_IGNORE) {
            return;
        }
        if (policy == LEME_ACTIVATION_URGENT) {
            entry->urgent = true;
            leme_publication_invalidate(server);
            return;
        }
        leme_output_set_focused(server, entry->output, true);
    }
    leme_tags_focus_id(tags, entry->tag_id);
    leme_view_refresh_tag_focus(server);
    leme_publication_invalidate(server);
}

void
leme_workspace_handle_requests(struct leme_server *server,
    struct wl_list *requests)
{
    struct wlr_ext_workspace_v1_request *request;

    if (leme_session_locked(server)) {
        return;
    }
    wl_list_for_each(request, requests, link) {
        if (request->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
            continue;
        }
        if (request->activate.workspace == NULL) {
            continue;
        }
        leme_workspace_activate(server, request->activate.workspace->data);
    }
}

static void
leme_workspace_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, workspace_commit);
    struct wlr_ext_workspace_v1_commit_event *event = data;

    leme_workspace_handle_requests(server, event->requests);
}

void
leme_workspace_release_output(struct leme_output *output)
{
    struct leme_server *server = output->server;
    struct leme_workspace_entry *entry;
    struct leme_workspace_entry *tmp;

    if (server == NULL || server->workspace_manager == NULL) {
        return;
    }
    wl_list_for_each_safe(entry, tmp, &server->workspace_entries, link) {
        if (entry->output == output) {
            leme_workspace_entry_destroy(entry);
        }
    }
    if (output->workspace_group != NULL) {
        wlr_ext_workspace_group_handle_v1_destroy(output->workspace_group);
        output->workspace_group = NULL;
    }
}

void
leme_workspace_reconcile(struct leme_server *server)
{
    struct leme_workspace_entry *entry;
    struct leme_workspace_entry *tmp;
    struct leme_output *output;

    if (server->workspace_manager == NULL) {
        return;
    }
    wl_list_for_each(entry, &server->workspace_entries, link) {
        entry->seen = false;
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output != NULL && output->wlr_output->enabled) {
            leme_workspace_sync_output(server, output);
        }
    }
    wl_list_for_each_safe(entry, tmp, &server->workspace_entries, link) {
        if (!entry->seen) {
            leme_workspace_entry_destroy(entry);
        }
    }
    wl_list_for_each(output, &server->outputs, link) {
        bool enabled = output->wlr_output != NULL &&
            output->wlr_output->enabled;

        if (!enabled && output->workspace_group != NULL) {
            wlr_ext_workspace_group_handle_v1_destroy(output->workspace_group);
            output->workspace_group = NULL;
        }
    }
}

bool
leme_workspace_init(struct leme_server *server)
{
    wl_list_init(&server->workspace_entries);
    server->workspace_manager =
        wlr_ext_workspace_manager_v1_create(server->display, 1);
    if (server->workspace_manager == NULL) {
        return false;
    }
    server->workspace_commit.notify = leme_workspace_handle_commit;
    wl_signal_add(&server->workspace_manager->events.commit,
        &server->workspace_commit);
    return true;
}

void
leme_workspace_finish(struct leme_server *server)
{
    struct leme_workspace_entry *entry;
    struct leme_workspace_entry *tmp;
    struct leme_output *output;

    if (server->workspace_manager == NULL) {
        return;
    }
    wl_list_remove(&server->workspace_commit.link);
    wl_list_for_each_safe(entry, tmp, &server->workspace_entries, link) {
        leme_workspace_entry_destroy(entry);
    }
    wl_list_for_each(output, &server->outputs, link) {
        output->workspace_group = NULL;
    }
    server->workspace_manager = NULL;
}
