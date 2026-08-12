#ifndef LEME_SCFG_INTERNAL_H
#define LEME_SCFG_INTERNAL_H

#include "config/scfg.h"

enum leme_scfg_token_kind {
    LEME_SCFG_TOKEN_WORD,
    LEME_SCFG_TOKEN_OPEN,
    LEME_SCFG_TOKEN_CLOSE,
    LEME_SCFG_TOKEN_NEWLINE,
    LEME_SCFG_TOKEN_END,
};

struct leme_scfg_token {
    enum leme_scfg_token_kind kind;
    char *value;
    struct leme_scfg_span span;
};

struct leme_scfg_tokens {
    struct leme_scfg_token *entries;
    size_t count;
};

bool leme_scfg_error_add(struct leme_scfg_result *result,
    struct leme_scfg_span span, const char *format, ...);
bool leme_scfg_lex(const struct leme_scfg_source *source,
    struct leme_scfg_tokens *tokens, struct leme_scfg_result *result);
void leme_scfg_tokens_finish(struct leme_scfg_tokens *tokens);

#endif
