#include "config/diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static bool
leme_diagnostics_grow(struct leme_diagnostics *diagnostics)
{
    struct leme_diagnostic *entries;
    size_t capacity;

    if (diagnostics->count < diagnostics->capacity) {
        return true;
    }
    capacity = diagnostics->capacity == 0 ? 8 : diagnostics->capacity * 2;
    if (capacity > LEME_DIAGNOSTICS_MAX) {
        capacity = LEME_DIAGNOSTICS_MAX;
    }
    entries = realloc(diagnostics->entries, capacity * sizeof(*entries));
    if (entries == NULL) {
        return false;
    }
    diagnostics->entries = entries;
    diagnostics->capacity = capacity;
    return true;
}

bool
leme_diagnostics_add(struct leme_diagnostics *diagnostics, int line,
    const char *format, ...)
{
    va_list arguments;
    va_list copy;
    char *message;
    int length;

    if (diagnostics->count >= LEME_DIAGNOSTICS_MAX) {
        diagnostics->truncated = true;
        return true;
    }
    if (!leme_diagnostics_grow(diagnostics)) {
        return false;
    }
    va_start(arguments, format);
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(arguments);
        return false;
    }
    message = malloc((size_t)length + 1);
    if (message == NULL) {
        va_end(arguments);
        return false;
    }
    vsnprintf(message, (size_t)length + 1, format, arguments);
    va_end(arguments);
    diagnostics->entries[diagnostics->count].message = message;
    diagnostics->entries[diagnostics->count].line = line;
    diagnostics->count++;
    return true;
}

void
leme_diagnostics_finish(struct leme_diagnostics *diagnostics)
{
    size_t index;

    for (index = 0; index < diagnostics->count; index++) {
        free(diagnostics->entries[index].message);
    }
    free(diagnostics->entries);
    *diagnostics = (struct leme_diagnostics){0};
}
