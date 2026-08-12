#ifndef LEME_DIAGNOSTICS_H
#define LEME_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>

#define LEME_DIAGNOSTICS_MAX 64

struct leme_diagnostic {
    char *message;
    int line;
};

struct leme_diagnostics {
    struct leme_diagnostic *entries;
    size_t count;
    size_t capacity;
    bool truncated;
};

bool leme_diagnostics_add(struct leme_diagnostics *diagnostics, int line,
    const char *format, ...);
void leme_diagnostics_finish(struct leme_diagnostics *diagnostics);

#endif
