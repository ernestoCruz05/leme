#include "ipc/ipc.h"

#include "config/internal.h"
#include "core/command.h"
#include "core/server.h"
#include "ipc/json.h"
#include "ipc/request.h"
#include "ipc/state.h"
#include "protocols/session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

#define LEME_IPC_MAX_CLIENTS 16
#define LEME_IPC_MAX_LINE 1024
#define LEME_IPC_MAX_OUTPUT 65536

struct leme_ipc_client {
    struct leme_ipc *ipc;
    int fd;
    struct wl_event_source *source;
    char input[LEME_IPC_MAX_LINE + 1];
    size_t input_length;
    char *output;
    size_t output_length;
    size_t output_capacity;
    bool subscribed;
    uint32_t events;
    struct wl_list link;
};

struct leme_ipc {
    struct leme_server *server;
    int fd;
    char *path;
    struct wl_event_source *source;
    struct wl_event_source *idle;
    struct wl_list clients;
    size_t client_count;
    struct leme_ipc_state snapshot;
    bool snapshot_valid;
    bool dirty;
};

static void
leme_ipc_client_destroy(struct leme_ipc_client *client)
{
    wl_list_remove(&client->link);
    client->ipc->client_count--;
    if (client->source != NULL) {
        wl_event_source_remove(client->source);
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client->output);
    free(client);
}

static bool
leme_ipc_client_flush(struct leme_ipc_client *client)
{
    while (client->output_length > 0) {
        ssize_t written = send(client->fd, client->output,
            client->output_length, MSG_NOSIGNAL);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        client->output_length -= (size_t)written;
        memmove(client->output, client->output + written,
            client->output_length);
    }
    return true;
}

static bool
leme_ipc_client_queue(struct leme_ipc_client *client,
    const char *text, size_t length)
{
    size_t needed;

    if (length > LEME_IPC_MAX_OUTPUT - client->output_length) {
        return false;
    }
    needed = client->output_length + length;
    if (needed > client->output_capacity) {
        size_t capacity = client->output_capacity == 0 ?
            1024 : client->output_capacity;
        char *data;

        while (capacity < needed) {
            capacity *= 2;
        }
        data = realloc(client->output, capacity);
        if (data == NULL) {
            return false;
        }
        client->output = data;
        client->output_capacity = capacity;
    }
    memcpy(client->output + client->output_length, text, length);
    client->output_length = needed;
    return leme_ipc_client_flush(client);
}

static bool
leme_ipc_client_send(struct leme_ipc_client *client, const char *text)
{
    return leme_ipc_client_queue(client, text, strlen(text)) &&
        leme_ipc_client_queue(client, "\n", 1);
}

static bool
leme_ipc_client_fail(struct leme_ipc_client *client, const char *format, ...)
{
    struct leme_json json;
    char message[256];
    va_list arguments;
    const char *text;
    bool sent;
    int written;

    va_start(arguments, format);
    written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return false;
    }
    leme_json_init(&json);
    leme_json_object_begin(&json);
    leme_json_key(&json, "ok");
    leme_json_bool(&json, false);
    leme_json_key(&json, "error");
    leme_json_string(&json, message);
    leme_json_object_end(&json);
    text = leme_json_result(&json);
    sent = text != NULL && leme_ipc_client_send(client, text);
    leme_json_finish(&json);
    return sent;
}

static bool
leme_ipc_command_refused(enum leme_command_type type)
{
    return type == LEME_COMMAND_SPAWN || type == LEME_COMMAND_QUIT ||
        type == LEME_COMMAND_SWITCH_VT;
}

static bool
leme_ipc_client_command(struct leme_ipc_client *client,
    char **tokens, size_t count)
{
    struct leme_command command = {0};
    char *detail = NULL;
    bool sent;

    if (leme_session_locked(client->ipc->server)) {
        return leme_ipc_client_fail(client, "%s", "session locked");
    }
    if (!leme_command_parse(&command, tokens, count, &detail)) {
        sent = leme_ipc_client_fail(client, "%s",
            detail == NULL ? "invalid command" : detail);
        free(detail);
        return sent;
    }
    if (leme_ipc_command_refused(command.type)) {
        sent = leme_ipc_client_fail(client, "refused: %s", tokens[0]);
    } else if (leme_command_execute(client->ipc->server, &command)) {
        sent = leme_ipc_client_send(client, "{\"ok\":true}");
    } else {
        sent = leme_ipc_client_fail(client, "%s", "command failed");
    }
    free(command.text);
    leme_config_free_argv(command.argv);
    return sent;
}

static bool
leme_ipc_client_get(struct leme_ipc_client *client, char **path,
    size_t path_length)
{
    struct leme_ipc_state state;
    struct leme_json json;
    char *error = NULL;
    const char *text;
    bool sent;

    if (!leme_ipc_state_capture(&state, client->ipc->server)) {
        return leme_ipc_client_fail(client, "%s", "out of memory");
    }
    leme_json_init(&json);
    leme_json_object_begin(&json);
    leme_json_key(&json, "ok");
    leme_json_bool(&json, true);
    leme_json_key(&json, "value");
    if (!leme_ipc_state_write(&state, &json, path, path_length, &error)) {
        leme_json_finish(&json);
        leme_ipc_state_finish(&state);
        sent = leme_ipc_client_fail(client, "%s",
            error == NULL ? "no such path" : error);
        free(error);
        return sent;
    }
    leme_json_object_end(&json);
    text = leme_json_result(&json);
    sent = text != NULL && leme_ipc_client_send(client, text);
    leme_json_finish(&json);
    leme_ipc_state_finish(&state);
    free(error);
    return sent;
}

static bool
leme_ipc_client_subscribe(struct leme_ipc_client *client,
    char **names, size_t count)
{
    uint32_t events = 0;
    size_t index;

    if (count == 0) {
        events = LEME_IPC_EVENT_ALL;
    }
    for (index = 0; index < count; index++) {
        uint32_t bit;

        if (!leme_ipc_event_from_name(names[index], &bit)) {
            return leme_ipc_client_fail(client, "no such event: %s",
                names[index]);
        }
        events |= bit;
    }
    client->events |= events;
    client->subscribed = true;
    return leme_ipc_client_send(client, "{\"ok\":true}");
}

static void
leme_ipc_emit(struct leme_ipc *ipc, uint32_t event, const char *text)
{
    struct leme_ipc_client *client;
    struct leme_ipc_client *tmp;

    wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
        if (!client->subscribed || (client->events & event) == 0) {
            continue;
        }
        if (!leme_ipc_client_send(client, text)) {
            leme_ipc_client_destroy(client);
        }
    }
}

static void
leme_ipc_emit_string(struct leme_ipc *ipc, uint32_t event,
    const char *name, const char *key, const char *value)
{
    struct leme_json json;
    const char *text;

    leme_json_init(&json);
    leme_json_object_begin(&json);
    leme_json_key(&json, "event");
    leme_json_string(&json, name);
    leme_json_key(&json, key);
    leme_json_string(&json, value);
    leme_json_object_end(&json);
    text = leme_json_result(&json);
    if (text != NULL) {
        leme_ipc_emit(ipc, event, text);
    }
    leme_json_finish(&json);
}

static bool
leme_ipc_text_changed(const char *previous, const char *current)
{
    if (previous == NULL || current == NULL) {
        return previous != current;
    }
    return strcmp(previous, current) != 0;
}

static void
leme_ipc_diff_workspaces(struct leme_ipc *ipc,
    const struct leme_ipc_state *previous,
    const struct leme_ipc_state *current)
{
    size_t index;

    for (index = 0; index < current->workspace_count; index++) {
        const struct leme_ipc_workspace *entry = &current->workspaces[index];
        const struct leme_ipc_workspace *before = NULL;
        struct leme_json json;
        const char *text;
        size_t search;

        for (search = 0; search < previous->workspace_count; search++) {
            if (strcmp(previous->workspaces[search].id, entry->id) == 0) {
                before = &previous->workspaces[search];
                break;
            }
        }
        if (before != NULL && before->layout == entry->layout) {
            continue;
        }
        leme_json_init(&json);
        leme_json_object_begin(&json);
        leme_json_key(&json, "event");
        leme_json_string(&json, "layout");
        leme_json_key(&json, "workspace");
        leme_json_string(&json, entry->id);
        leme_json_key(&json, "layout");
        leme_json_string(&json, leme_ipc_layout_name(entry->layout));
        leme_json_object_end(&json);
        text = leme_json_result(&json);
        if (text != NULL) {
            leme_ipc_emit(ipc, LEME_IPC_EVENT_LAYOUT, text);
        }
        leme_json_finish(&json);
    }
}

static void
leme_ipc_diff(struct leme_ipc *ipc, const struct leme_ipc_state *previous,
    const struct leme_ipc_state *current)
{
    if (leme_ipc_text_changed(previous->mode, current->mode)) {
        leme_ipc_emit_string(ipc, LEME_IPC_EVENT_MODE, "mode", "mode",
            current->mode);
    }
    if (leme_ipc_text_changed(previous->keyboard_active,
            current->keyboard_active)) {
        leme_ipc_emit_string(ipc, LEME_IPC_EVENT_KEYBOARD_LAYOUT,
            "keyboard_layout", "layout", current->keyboard_active);
    }
    if (leme_ipc_text_changed(previous->focused_output,
            current->focused_output)) {
        leme_ipc_emit_string(ipc, LEME_IPC_EVENT_FOCUSED_OUTPUT,
            "focused_output", "output", current->focused_output);
    }
    if (previous->has_focused_view != current->has_focused_view ||
            previous->focused_floating != current->focused_floating ||
            previous->focused_scratchpad != current->focused_scratchpad) {
        struct leme_json json;
        const char *text;

        leme_json_init(&json);
        leme_json_object_begin(&json);
        leme_json_key(&json, "event");
        leme_json_string(&json, "view");
        leme_json_key(&json, "floating");
        if (current->has_focused_view) {
            leme_json_bool(&json, current->focused_floating);
        } else {
            leme_json_null(&json);
        }
        leme_json_key(&json, "scratchpad");
        if (current->has_focused_view) {
            leme_json_bool(&json, current->focused_scratchpad);
        } else {
            leme_json_null(&json);
        }
        leme_json_object_end(&json);
        text = leme_json_result(&json);
        if (text != NULL) {
            leme_ipc_emit(ipc, LEME_IPC_EVENT_VIEW, text);
        }
        leme_json_finish(&json);
    }
    if (previous->config_diagnostic_count != current->config_diagnostic_count ||
            previous->config_truncated != current->config_truncated ||
            leme_ipc_text_changed(previous->config_path,
                current->config_path)) {
        struct leme_json json;
        const char *text;

        leme_json_init(&json);
        leme_json_object_begin(&json);
        leme_json_key(&json, "event");
        leme_json_string(&json, "config");
        leme_json_key(&json, "diagnostics");
        leme_json_number(&json, (long)current->config_diagnostic_count);
        leme_json_key(&json, "truncated");
        leme_json_bool(&json, current->config_truncated);
        leme_json_object_end(&json);
        text = leme_json_result(&json);
        if (text != NULL) {
            leme_ipc_emit(ipc, LEME_IPC_EVENT_CONFIG, text);
        }
        leme_json_finish(&json);
    }
    leme_ipc_diff_workspaces(ipc, previous, current);
}

static void
leme_ipc_reconcile(struct leme_ipc *ipc)
{
    struct leme_ipc_state current;

    if (leme_session_locked(ipc->server)) {
        return;
    }
    ipc->dirty = false;
    if (!leme_ipc_state_capture(&current, ipc->server)) {
        return;
    }
    if (ipc->snapshot_valid) {
        leme_ipc_diff(ipc, &ipc->snapshot, &current);
        leme_ipc_state_finish(&ipc->snapshot);
    }
    ipc->snapshot = current;
    ipc->snapshot_valid = true;
}

static void
leme_ipc_handle_idle(void *data)
{
    struct leme_ipc *ipc = data;

    ipc->idle = NULL;
    leme_ipc_reconcile(ipc);
}

static bool
leme_ipc_client_dispatch(struct leme_ipc_client *client, char *line)
{
    char *tokens[LEME_REQUEST_MAX_TOKENS];
    size_t count = leme_request_tokenize(line, tokens,
        LEME_REQUEST_MAX_TOKENS);

    if (count == 0) {
        return true;
    }
    if (strcmp(tokens[0], "get") == 0) {
        return leme_ipc_client_get(client, &tokens[1], count - 1);
    }
    if (strcmp(tokens[0], "subscribe") == 0) {
        return leme_ipc_client_subscribe(client, &tokens[1], count - 1);
    }
    return leme_ipc_client_command(client, tokens, count);
}

static int
leme_ipc_handle_client(int fd, uint32_t mask, void *data)
{
    struct leme_ipc_client *client = data;
    char buffer[LEME_IPC_MAX_LINE];
    ssize_t received;

    (void)fd;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        leme_ipc_client_destroy(client);
        return 0;
    }
    received = recv(client->fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
        leme_ipc_client_destroy(client);
        return 0;
    }
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        leme_ipc_client_destroy(client);
        return 0;
    }
    for (ssize_t index = 0; index < received; index++) {
        char character = buffer[index];

        if (character == '\n') {
            client->input[client->input_length] = '\0';
            client->input_length = 0;
            if (!leme_ipc_client_dispatch(client, client->input)) {
                leme_ipc_client_destroy(client);
                return 0;
            }
            continue;
        }
        if (client->input_length >= LEME_IPC_MAX_LINE) {
            wlr_log(WLR_ERROR, "%s",
                "leme: control interface request exceeds the line limit");
            leme_ipc_client_destroy(client);
            return 0;
        }
        client->input[client->input_length++] = character;
    }
    return 0;
}

static int
leme_ipc_handle_listen(int fd, uint32_t mask, void *data)
{
    struct leme_ipc *ipc = data;
    struct leme_ipc_client *client;
    struct wl_event_loop *loop;
    int client_fd;

    (void)fd;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        return 0;
    }
    client_fd = accept(ipc->fd, NULL, NULL);
    if (client_fd < 0) {
        return 0;
    }
    if (fcntl(client_fd, F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(client_fd, F_SETFL, O_NONBLOCK) != 0) {
        close(client_fd);
        return 0;
    }
    if (ipc->client_count >= LEME_IPC_MAX_CLIENTS) {
        close(client_fd);
        return 0;
    }
    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(client_fd);
        return 0;
    }
    client->ipc = ipc;
    client->fd = client_fd;
    loop = wl_display_get_event_loop(ipc->server->display);
    client->source = wl_event_loop_add_fd(loop, client_fd, WL_EVENT_READABLE,
        leme_ipc_handle_client, client);
    if (client->source == NULL) {
        close(client_fd);
        free(client);
        return 0;
    }
    wl_list_insert(&ipc->clients, &client->link);
    ipc->client_count++;
    return 0;
}

static bool
leme_ipc_socket_in_use(const char *path)
{
    struct sockaddr_un address = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool live;

    if (fd < 0) {
        return false;
    }
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path));
    live = connect(fd, (const struct sockaddr *)&address,
        sizeof(address)) == 0;
    close(fd);
    return live;
}

static bool
leme_ipc_build_path(const struct leme_server *server, char *path, size_t size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int written;

    if (runtime == NULL || runtime[0] == '\0' || server->socket == NULL) {
        return false;
    }
    written = snprintf(path, size, "%s/leme-%s.sock", runtime,
        server->socket);
    return written >= 0 && (size_t)written < size;
}

bool
leme_ipc_init(struct leme_server *server)
{
    struct sockaddr_un address = {0};
    struct leme_ipc *ipc;
    struct wl_event_loop *loop;
    char path[PATH_MAX];
    mode_t saved;
    bool bound = false;

    if (!leme_ipc_build_path(server, path, sizeof(path))) {
        wlr_log(WLR_INFO, "%s",
            "leme: no runtime directory, control interface disabled");
        return true;
    }
    if (strlen(path) >= sizeof(address.sun_path)) {
        wlr_log(WLR_ERROR, "%s", "leme: control interface path is too long");
        return true;
    }
    if (access(path, F_OK) == 0) {
        if (leme_ipc_socket_in_use(path)) {
            wlr_log(WLR_ERROR,
                "leme: %s is already served, control interface disabled",
                path);
            return true;
        }
        unlink(path);
    }

    ipc = calloc(1, sizeof(*ipc));
    if (ipc == NULL) {
        return false;
    }
    ipc->server = server;
    ipc->fd = -1;
    wl_list_init(&ipc->clients);
    ipc->path = strdup(path);
    if (ipc->path == NULL) {
        goto error;
    }
    ipc->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (ipc->fd < 0) {
        goto error;
    }
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path));
    saved = umask(0077);
    bound = bind(ipc->fd, (const struct sockaddr *)&address,
        sizeof(address)) == 0;
    umask(saved);
    if (!bound) {
        goto error;
    }
    if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
        goto error;
    }
    if (listen(ipc->fd, LEME_IPC_MAX_CLIENTS) != 0) {
        goto error;
    }
    loop = wl_display_get_event_loop(server->display);
    ipc->source = wl_event_loop_add_fd(loop, ipc->fd, WL_EVENT_READABLE,
        leme_ipc_handle_listen, ipc);
    if (ipc->source == NULL) {
        goto error;
    }
    if (setenv("LEME_SOCKET", path, 1) != 0) {
        goto error;
    }
    server->ipc = ipc;
    wlr_log(WLR_INFO, "leme: LEME_SOCKET=%s", path);
    return true;

error:
    wlr_log(WLR_ERROR, "%s", "leme: failed to create the control interface");
    if (ipc->source != NULL) {
        wl_event_source_remove(ipc->source);
    }
    if (ipc->fd >= 0) {
        close(ipc->fd);
    }
    if (bound) {
        unlink(path);
    }
    free(ipc->path);
    free(ipc);
    return true;
}

void
leme_ipc_finish(struct leme_server *server)
{
    struct leme_ipc *ipc = server->ipc;
    struct leme_ipc_client *client;
    struct leme_ipc_client *tmp;

    if (ipc == NULL) {
        return;
    }
    wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
        leme_ipc_client_destroy(client);
    }
    if (ipc->idle != NULL) {
        wl_event_source_remove(ipc->idle);
    }
    if (ipc->source != NULL) {
        wl_event_source_remove(ipc->source);
    }
    if (ipc->fd >= 0) {
        close(ipc->fd);
    }
    if (ipc->snapshot_valid) {
        leme_ipc_state_finish(&ipc->snapshot);
    }
    if (ipc->path != NULL) {
        unlink(ipc->path);
        free(ipc->path);
    }
    unsetenv("LEME_SOCKET");
    free(ipc);
    server->ipc = NULL;
}

void
leme_ipc_invalidate(struct leme_server *server)
{
    struct leme_ipc *ipc = server->ipc;
    struct wl_event_loop *loop;

    if (ipc == NULL || server->display == NULL) {
        return;
    }
    ipc->dirty = true;
    if (ipc->idle != NULL) {
        return;
    }
    loop = wl_display_get_event_loop(server->display);
    ipc->idle = wl_event_loop_add_idle(loop, leme_ipc_handle_idle, ipc);
    if (ipc->idle == NULL) {
        leme_ipc_reconcile(ipc);
    }
}

const char *
leme_ipc_socket_path(const struct leme_server *server)
{
    return server->ipc == NULL ? NULL : server->ipc->path;
}
