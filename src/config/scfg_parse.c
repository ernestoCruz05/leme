#include "config/scfg_internal.h"

#include <stdlib.h>
#include <string.h>

#define LEME_SCFG_MAX_DEPTH 64

struct leme_scfg_parser {
    const struct leme_scfg_source *source;
    struct leme_scfg_tokens *tokens;
    struct leme_scfg_result *result;
    size_t index;
};

static bool leme_scfg_parse_block(struct leme_scfg_parser *parser,
    struct leme_scfg_block *block, int depth, bool *closed);

static const struct leme_scfg_token *
leme_scfg_peek(const struct leme_scfg_parser *parser)
{
    return &parser->tokens->entries[parser->index];
}

static void
leme_scfg_advance(struct leme_scfg_parser *parser)
{
    if (leme_scfg_peek(parser)->kind != LEME_SCFG_TOKEN_END) {
        parser->index++;
    }
}

/*
 * A directiva entra no bloco antes de ser preenchida. Se faltar memória a
 * meio, fica lá meio construída e o leme_scfg_result_finish liberta-a.
 */
static bool
leme_scfg_block_push(struct leme_scfg_block *block,
    struct leme_scfg_directive **directive)
{
    struct leme_scfg_directive *entries = realloc(block->directives,
        (block->directives_len + 1) * sizeof(*entries));

    if (entries == NULL) {
        return false;
    }
    block->directives = entries;
    *directive = &block->directives[block->directives_len];
    **directive = (struct leme_scfg_directive){0};
    block->directives_len++;
    return true;
}

static bool
leme_scfg_param_push(struct leme_scfg_directive *directive,
    const struct leme_scfg_token *token)
{
    char **params;
    struct leme_scfg_span *spans;
    char *value = strdup(token->value);

    if (value == NULL) {
        return false;
    }
    params = realloc(directive->params,
        (directive->params_len + 1) * sizeof(*params));
    if (params == NULL) {
        free(value);
        return false;
    }
    directive->params = params;
    spans = realloc(directive->param_spans,
        (directive->params_len + 1) * sizeof(*spans));
    if (spans == NULL) {
        free(value);
        return false;
    }
    directive->param_spans = spans;
    directive->params[directive->params_len] = value;
    directive->param_spans[directive->params_len] = token->span;
    directive->params_len++;
    return true;
}

static void
leme_scfg_skip_block(struct leme_scfg_parser *parser)
{
    int depth = 1;

    while (depth > 0) {
        const struct leme_scfg_token *token = leme_scfg_peek(parser);

        if (token->kind == LEME_SCFG_TOKEN_END) {
            return;
        }
        if (token->kind == LEME_SCFG_TOKEN_OPEN) {
            depth++;
        } else if (token->kind == LEME_SCFG_TOKEN_CLOSE) {
            depth--;
        }
        leme_scfg_advance(parser);
    }
}

static size_t
leme_scfg_directive_end(const struct leme_scfg_parser *parser)
{
    size_t back = parser->index;

    while (back > 0 && parser->tokens->entries[back - 1].kind ==
            LEME_SCFG_TOKEN_NEWLINE) {
        back--;
    }
    if (back == 0) {
        return 0;
    }
    return parser->tokens->entries[back - 1].span.offset +
        parser->tokens->entries[back - 1].span.length;
}

static bool
leme_scfg_parse_directive(struct leme_scfg_parser *parser,
    struct leme_scfg_directive *directive, int depth)
{
    const struct leme_scfg_token *token = leme_scfg_peek(parser);
    size_t end;
    int line = 0;
    int column = 0;

    directive->name = strdup(token->value);
    if (directive->name == NULL) {
        return false;
    }
    directive->name_span = token->span;
    leme_scfg_source_locate(parser->source, token->span, &line, &column);
    directive->lineno = line;
    leme_scfg_advance(parser);

    for (;;) {
        token = leme_scfg_peek(parser);
        if (token->kind == LEME_SCFG_TOKEN_WORD) {
            if (!leme_scfg_param_push(directive, token)) {
                return false;
            }
            leme_scfg_advance(parser);
            continue;
        }
        if (token->kind == LEME_SCFG_TOKEN_OPEN) {
            struct leme_scfg_span open = token->span;
            bool closed = false;

            leme_scfg_advance(parser);
            if (depth + 1 >= LEME_SCFG_MAX_DEPTH) {
                leme_scfg_error_add(parser->result, open,
                    "blocks are nested too deeply");
                leme_scfg_skip_block(parser);
                break;
            }
            if (!leme_scfg_parse_block(parser, &directive->children,
                    depth + 1, &closed)) {
                return false;
            }
            if (!closed) {
                leme_scfg_error_add(parser->result, open,
                    "this block was never closed");
            }
            break;
        }
        if (token->kind == LEME_SCFG_TOKEN_NEWLINE) {
            leme_scfg_advance(parser);
            break;
        }
        break;
    }
    end = leme_scfg_directive_end(parser);
    directive->span = (struct leme_scfg_span){
        .offset = directive->name_span.offset,
        .length = end > directive->name_span.offset ?
            end - directive->name_span.offset : 0,
    };
    return true;
}

static bool
leme_scfg_parse_block(struct leme_scfg_parser *parser,
    struct leme_scfg_block *block, int depth, bool *closed)
{
    *closed = false;
    for (;;) {
        const struct leme_scfg_token *token = leme_scfg_peek(parser);
        struct leme_scfg_directive *directive;

        if (token->kind == LEME_SCFG_TOKEN_NEWLINE) {
            leme_scfg_advance(parser);
            continue;
        }
        if (token->kind == LEME_SCFG_TOKEN_END) {
            return true;
        }
        if (token->kind == LEME_SCFG_TOKEN_CLOSE) {
            if (depth > 0) {
                leme_scfg_advance(parser);
                *closed = true;
                return true;
            }
            leme_scfg_error_add(parser->result, token->span,
                "unexpected '}'");
            leme_scfg_advance(parser);
            continue;
        }
        if (token->kind == LEME_SCFG_TOKEN_OPEN) {
            leme_scfg_error_add(parser->result, token->span,
                "expected a directive name before '{'");
            leme_scfg_advance(parser);
            leme_scfg_skip_block(parser);
            continue;
        }
        if (!leme_scfg_block_push(block, &directive)) {
            return false;
        }
        if (!leme_scfg_parse_directive(parser, directive, depth)) {
            return false;
        }
    }
}

bool
leme_scfg_parse(const struct leme_scfg_source *source,
    struct leme_scfg_result *result)
{
    struct leme_scfg_tokens tokens = {0};
    struct leme_scfg_parser parser;
    bool closed = false;
    bool parsed;

    leme_scfg_lex(source, &tokens, result);
    if (tokens.count == 0 || tokens.entries[tokens.count - 1].kind !=
            LEME_SCFG_TOKEN_END) {
        leme_scfg_tokens_finish(&tokens);
        return false;
    }
    parser = (struct leme_scfg_parser){
        .source = source,
        .tokens = &tokens,
        .result = result,
    };
    parsed = leme_scfg_parse_block(&parser, &result->block, 0, &closed);
    leme_scfg_tokens_finish(&tokens);
    return parsed && result->error_count == 0;
}
