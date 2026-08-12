#ifndef LEME_SCFG_H
#define LEME_SCFG_H

#include <stdbool.h>
#include <stddef.h>

#define LEME_SCFG_MAX_ERRORS 20

struct leme_scfg_span {
    size_t offset;
    size_t length;
};

struct leme_scfg_source {
    char *data;
    size_t length;
    size_t *line_offsets;
    size_t line_count;
};

bool leme_scfg_source_load(struct leme_scfg_source *source, const char *path);
void leme_scfg_source_finish(struct leme_scfg_source *source);
void leme_scfg_source_locate(const struct leme_scfg_source *source,
    struct leme_scfg_span span, int *line, int *column);
bool leme_scfg_source_line_text(const struct leme_scfg_source *source,
    int line, const char **text, size_t *length);

struct leme_scfg_error {
    struct leme_scfg_span span;
    char *message;
};

struct leme_scfg_block {
    struct leme_scfg_directive *directives;
    size_t directives_len;
};

struct leme_scfg_directive {
    char *name;
    char **params;
    size_t params_len;
    struct leme_scfg_block children;
    int lineno;
    struct leme_scfg_span span;
    struct leme_scfg_span name_span;
    struct leme_scfg_span *param_spans;
};

struct leme_scfg_result {
    struct leme_scfg_block block;
    struct leme_scfg_error *errors;
    size_t error_count;
    bool truncated;
};

bool leme_scfg_parse(const struct leme_scfg_source *source,
    struct leme_scfg_result *result);
void leme_scfg_result_finish(struct leme_scfg_result *result);
char *leme_scfg_render_error(const struct leme_scfg_source *source,
    const char *path, const struct leme_scfg_error *error);

#endif
