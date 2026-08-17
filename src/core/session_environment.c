#include "core/session_environment.h"

#include "config/config.h"
#include "core/server.h"
#include "shell/xwayland.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

extern char **environ;

static void
leme_session_environment_error(void)
{
    wlr_log(WLR_ERROR, "%s",
        "leme: failed to update D-Bus activation environment");
}

static bool
leme_session_environment_set(struct leme_server *server)
{
    const char *display;

    if (server->socket == NULL ||
            setenv("WAYLAND_DISPLAY", server->socket, 1) < 0 ||
            setenv("XDG_CURRENT_DESKTOP", "leme", 1) < 0 ||
            setenv("XDG_SESSION_DESKTOP", "leme", 1) < 0 ||
            setenv("XDG_SESSION_TYPE", "wayland", 1) < 0) {
        return false;
    }
    display = leme_xwayland_display(server);
    if (display != NULL) {
        return setenv("DISPLAY", display, 1) == 0;
    }
    return unsetenv("DISPLAY") == 0;
}

/* O bloco env só é aplicado depois do fork; o Xwayland herda este ambiente. */
void
leme_session_environment_cursor(const struct leme_server *server)
{
    char size[16];
    int written;

    if (server == NULL || server->config == NULL) {
        return;
    }
    written = snprintf(size, sizeof(size), "%d", server->config->cursor.size);
    if (written < 0 || (size_t)written >= sizeof(size) ||
            setenv("XCURSOR_SIZE", size, 1) < 0) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to export XCURSOR_SIZE");
        return;
    }
    if (server->config->cursor.theme != NULL &&
            setenv("XCURSOR_THEME", server->config->cursor.theme, 1) < 0) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to export XCURSOR_THEME");
    }
}

static bool
leme_session_environment_has_systemd(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    int length;

    if (runtime == NULL || runtime[0] == '\0') {
        return false;
    }
    length = snprintf(path, sizeof(path), "%s/systemd/private", runtime);
    return length >= 0 && (size_t)length < sizeof(path) &&
        access(path, F_OK) == 0;
}

static bool
leme_session_environment_run(const char *display)
{
    static char command[] = "dbus-update-activation-environment";
    static char systemd_flag[] = "--systemd";
    static char wayland_display[] = "WAYLAND_DISPLAY";
    static char display_variable[] = "DISPLAY";
    static char current_desktop[] = "XDG_CURRENT_DESKTOP";
    static char session_desktop[] = "XDG_SESSION_DESKTOP";
    static char session_type[] = "XDG_SESSION_TYPE";
    static char leme_socket[] = "LEME_SOCKET";
    static char cursor_size[] = "XCURSOR_SIZE";
    static char cursor_theme[] = "XCURSOR_THEME";
    char *arguments[12];
    size_t count = 0;
    pid_t process;
    int result;
    int status;

    arguments[count++] = command;
    if (leme_session_environment_has_systemd()) {
        arguments[count++] = systemd_flag;
    }
    arguments[count++] = wayland_display;
    if (display != NULL) {
        arguments[count++] = display_variable;
    }
    arguments[count++] = current_desktop;
    arguments[count++] = session_desktop;
    arguments[count++] = session_type;
    if (getenv("LEME_SOCKET") != NULL) {
        arguments[count++] = leme_socket;
    }
    if (getenv("XCURSOR_SIZE") != NULL) {
        arguments[count++] = cursor_size;
    }
    if (getenv("XCURSOR_THEME") != NULL) {
        arguments[count++] = cursor_theme;
    }
    arguments[count] = NULL;

    result = posix_spawnp(&process, arguments[0], NULL, NULL,
        arguments, environ);
    if (result != 0) {
        return false;
    }
    while (waitpid(process, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

void
leme_session_environment_publish(struct leme_server *server)
{
    const char *bus;
    const char *display;

    if (server == NULL || server->session == NULL) {
        return;
    }
    if (!leme_session_environment_set(server)) {
        leme_session_environment_error();
        return;
    }
    bus = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus == NULL || bus[0] == '\0') {
        wlr_log(WLR_ERROR, "%s",
            "leme: D-Bus session bus unavailable; portal activation environment not updated");
        return;
    }
    display = leme_xwayland_display(server);
    if (!leme_session_environment_run(display)) {
        leme_session_environment_error();
    }
}
