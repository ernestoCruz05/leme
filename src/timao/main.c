#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TIMAO_MAX_LINE 1024
#define TIMAO_MAX_REPLY 65536

static bool
timao_socket_path(char *path, size_t size)
{
    const char *direct = getenv("LEME_SOCKET");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    int written;

    if (direct != NULL && direct[0] != '\0') {
        written = snprintf(path, size, "%s", direct);
    } else if (runtime != NULL && display != NULL) {
        written = snprintf(path, size, "%s/leme-%s.sock", runtime, display);
    } else {
        return false;
    }
    return written >= 0 && (size_t)written < size;
}

static int
timao_connect(const char *path)
{
    struct sockaddr_un address = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(address.sun_path, path, strlen(path));
    if (connect(fd, (const struct sockaddr *)&address,
            sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool
timao_send_line(int fd, const char *line)
{
    size_t remaining = strlen(line);
    const char *cursor = line;

    while (remaining > 0) {
        ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);

        if (written <= 0) {
            return false;
        }
        remaining -= (size_t)written;
        cursor += written;
    }
    return true;
}

static bool
timao_read_line(int fd, char *buffer, size_t size)
{
    size_t length = 0;

    while (length + 1 < size) {
        ssize_t received = recv(fd, buffer + length, 1, 0);

        if (received <= 0) {
            break;
        }
        if (buffer[length] == '\n') {
            buffer[length] = '\0';
            return true;
        }
        length++;
    }
    buffer[length] = '\0';
    return length > 0;
}

static void
timao_print_unquoted(const char *start, const char *end)
{
    const char *cursor;

    for (cursor = start; cursor < end; cursor++) {
        if (*cursor != '\\' || cursor + 1 >= end) {
            putchar(*cursor);
            continue;
        }
        cursor++;
        switch (*cursor) {
        case 'n':
            putchar('\n');
            break;
        case 't':
            putchar('\t');
            break;
        case 'r':
            putchar('\r');
            break;
        case 'b':
            putchar('\b');
            break;
        case 'f':
            putchar('\f');
            break;
        default:
            putchar(*cursor);
            break;
        }
    }
}

static const char *
timao_scan_value(const char *cursor, const char **end)
{
    int depth = 0;
    bool in_string = false;

    for (*end = cursor; **end != '\0'; (*end)++) {
        char character = **end;

        if (in_string) {
            if (character == '\\' && *(*end + 1) != '\0') {
                (*end)++;
            } else if (character == '"') {
                in_string = false;
                if (depth == 0) {
                    (*end)++;
                    return cursor;
                }
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
            continue;
        }
        if (character == '{' || character == '[') {
            depth++;
        } else if (character == '}' || character == ']') {
            depth--;
            if (depth <= 0) {
                (*end)++;
                return cursor;
            }
        } else if (depth == 0 && (character == ',' || character == '}')) {
            return cursor;
        }
    }
    return cursor;
}

static void
timao_print_value(const char *start, const char *end, bool raw)
{
    if (!raw && start < end && *start == '"' && *(end - 1) == '"') {
        timao_print_unquoted(start + 1, end - 1);
        putchar('\n');
        return;
    }
    fwrite(start, 1, (size_t)(end - start), stdout);
    putchar('\n');
}

static int
timao_print_reply(const char *reply, bool raw)
{
    const char *value;
    const char *end;

    if (strstr(reply, "\"ok\":false") != NULL) {
        const char *error = strstr(reply, "\"error\":");

        if (error != NULL) {
            const char *start = timao_scan_value(error + 8, &end);
            bool quoted = end - start >= 2 && *start == '"' &&
                *(end - 1) == '"';

            fputs("timao: ", stderr);
            if (quoted) {
                fwrite(start + 1, 1, (size_t)(end - start - 2), stderr);
            } else if (end > start) {
                fwrite(start, 1, (size_t)(end - start), stderr);
            }
            fputc('\n', stderr);
        } else {
            fputs("timao: request failed\n", stderr);
        }
        return 1;
    }
    value = strstr(reply, "\"value\":");
    if (value == NULL) {
        return 0;
    }
    value = timao_scan_value(value + 8, &end);
    if (!raw && *value == '{') {
        const char *cursor = value + 1;

        while (*cursor != '\0' && *cursor != '}') {
            const char *member_end;
            const char *member;

            while (*cursor == ',' || *cursor == ' ') {
                cursor++;
            }
            if (*cursor != '"') {
                break;
            }
            timao_scan_value(cursor, &member_end);
            cursor = member_end;
            if (*cursor != ':') {
                break;
            }
            cursor++;
            member = timao_scan_value(cursor, &member_end);
            timao_print_value(member, member_end, false);
            cursor = member_end;
        }
        return 0;
    }
    timao_print_value(value, end, raw);
    return 0;
}

int
main(int argc, char **argv)
{
    char path[512];
    char line[TIMAO_MAX_LINE + 2];
    char *reply;
    bool raw = false;
    bool stream = false;
    size_t length = 0;
    int first = 1;
    int fd;
    int status;
    int index;

    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        raw = true;
        first = 2;
    }
    if (first >= argc) {
        fputs("usage: timao [--json] REQUEST [ARGUMENT...]\n", stderr);
        fputs("       timao sub [EVENT...]\n", stderr);
        return 2;
    }
    if (strcmp(argv[first], "sub") == 0) {
        stream = true;
    }
    length = (size_t)snprintf(line, sizeof(line), "%s",
        stream ? "subscribe" : argv[first]);
    for (index = first + 1; index < argc; index++) {
        int written = snprintf(line + length, sizeof(line) - length, " %s",
            argv[index]);

        if (written < 0 || (size_t)written >= sizeof(line) - length) {
            fputs("timao: request is too long\n", stderr);
            return 2;
        }
        length += (size_t)written;
    }
    if (length + 2 > sizeof(line)) {
        fputs("timao: request is too long\n", stderr);
        return 2;
    }
    line[length] = '\n';
    line[length + 1] = '\0';

    if (!timao_socket_path(path, sizeof(path))) {
        fputs("timao: set LEME_SOCKET, or run inside a Leme session\n",
            stderr);
        return 2;
    }
    fd = timao_connect(path);
    if (fd < 0) {
        fprintf(stderr, "timao: cannot connect to %s\n", path);
        return 2;
    }
    reply = malloc(TIMAO_MAX_REPLY);
    if (reply == NULL) {
        close(fd);
        return 2;
    }
    if (!timao_send_line(fd, line)) {
        fputs("timao: failed to send the request\n", stderr);
        free(reply);
        close(fd);
        return 2;
    }
    if (!timao_read_line(fd, reply, TIMAO_MAX_REPLY)) {
        fputs("timao: no reply\n", stderr);
        free(reply);
        close(fd);
        return 2;
    }
    status = timao_print_reply(reply, raw);
    if (stream && status == 0) {
        while (timao_read_line(fd, reply, TIMAO_MAX_REPLY)) {
            puts(reply);
            fflush(stdout);
        }
    }
    free(reply);
    close(fd);
    return status;
}
