#include "ipc/request.h"

static size_t
leme_request_split(char *text, char **parts, size_t capacity,
    char first, char second)
{
    size_t count = 0;
    char *cursor = text;

    while (*cursor != '\0') {
        char *start;

        while (*cursor == first || *cursor == second) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != first && *cursor != second) {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }
        if (count < capacity) {
            parts[count] = start;
        }
        count++;
    }
    return count > capacity ? capacity : count;
}

size_t
leme_request_tokenize(char *line, char **tokens, size_t capacity)
{
    return leme_request_split(line, tokens, capacity, ' ', '\t');
}

size_t
leme_request_fields(char *token, char **fields, size_t capacity)
{
    return leme_request_split(token, fields, capacity, ',', ',');
}
