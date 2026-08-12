#include "config/scfg_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct leme_scfg_text {
    char *data;
    size_t length;
    size_t capacity;
};

struct leme_scfg_lexer {
    const struct leme_scfg_source *source;
    struct leme_scfg_tokens *tokens;
    struct leme_scfg_result *result;
    size_t index;
};

static bool
leme_scfg_text_push(struct leme_scfg_text *text, char value)
{
    if (text->length + 1 > text->capacity) {
        size_t capacity = text->capacity == 0 ? 32 : text->capacity * 2;
        char *data = realloc(text->data, capacity);

        if (data == NULL) {
            return false;
        }
        text->data = data;
        text->capacity = capacity;
    }
    text->data[text->length++] = value;
    return true;
}

static bool
leme_scfg_token_push(struct leme_scfg_tokens *tokens,
    enum leme_scfg_token_kind kind, char *value, struct leme_scfg_span span)
{
    struct leme_scfg_token *entries = realloc(tokens->entries,
        (tokens->count + 1) * sizeof(*entries));

    if (entries == NULL) {
        free(value);
        return false;
    }
    tokens->entries = entries;
    tokens->entries[tokens->count] = (struct leme_scfg_token){
        .kind = kind,
        .value = value,
        .span = span,
    };
    tokens->count++;
    return true;
}

bool
leme_scfg_error_add(struct leme_scfg_result *result,
    struct leme_scfg_span span, const char *format, ...)
{
    va_list args;
    va_list measure;
    struct leme_scfg_error *errors;
    char *message;
    int needed;

    if (result->error_count >= LEME_SCFG_MAX_ERRORS) {
        result->truncated = true;
        return false;
    }
    va_start(args, format);
    va_copy(measure, args);
    needed = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    message = malloc((size_t)needed + 1);
    if (message == NULL) {
        va_end(args);
        return false;
    }
    vsnprintf(message, (size_t)needed + 1, format, args);
    va_end(args);
    errors = realloc(result->errors,
        (result->error_count + 1) * sizeof(*errors));
    if (errors == NULL) {
        free(message);
        return false;
    }
    result->errors = errors;
    result->errors[result->error_count] = (struct leme_scfg_error){
        .span = span,
        .message = message,
    };
    result->error_count++;
    return true;
}

/*
 * O '#' só abre comentário quando é seguido de espaço ou fim de linha. As
 * cores escrevem-se #RRGGBB sem aspas, e sem esta condição a linha
 * border_active #296bb8 perderia o valor.
 */
static bool
leme_scfg_is_comment(const struct leme_scfg_source *source, size_t index)
{
    char next;

    if (source->data[index] != '#') {
        return false;
    }
    if (index + 1 >= source->length) {
        return true;
    }
    next = source->data[index + 1];
    return next == ' ' || next == '\t' || next == '\n';
}

static void
leme_scfg_skip_line(struct leme_scfg_lexer *lexer)
{
    while (lexer->index < lexer->source->length &&
            lexer->source->data[lexer->index] != '\n') {
        lexer->index++;
    }
}

static bool
leme_scfg_emit_word(struct leme_scfg_lexer *lexer,
    struct leme_scfg_text *text, struct leme_scfg_span span)
{
    if (!leme_scfg_text_push(text, '\0')) {
        free(text->data);
        return false;
    }
    return leme_scfg_token_push(lexer->tokens, LEME_SCFG_TOKEN_WORD,
        text->data, span);
}

static bool
leme_scfg_lex_atom(struct leme_scfg_lexer *lexer)
{
    const struct leme_scfg_source *source = lexer->source;
    struct leme_scfg_text text = {0};
    size_t start = lexer->index;
    struct leme_scfg_span span;
    bool failed = false;

    while (lexer->index < source->length) {
        char value = source->data[lexer->index];

        if (value == ' ' || value == '\t' || value == '\n' ||
                value == ';' ||
                leme_scfg_is_comment(source, lexer->index)) {
            break;
        }
        if (value == '"' || value == '\'' || value == '{' || value == '}') {
            span = (struct leme_scfg_span){
                .offset = lexer->index,
                .length = 1,
            };
            leme_scfg_error_add(lexer->result, span,
                "unexpected '%c' in a word", value);
            failed = true;
            break;
        }
        if (value == '\\') {
            lexer->index++;
            if (lexer->index >= source->length ||
                    source->data[lexer->index] == '\n') {
                span = (struct leme_scfg_span){
                    .offset = lexer->index - 1,
                    .length = 1,
                };
                leme_scfg_error_add(lexer->result, span,
                    "a backslash cannot escape the end of a line");
                failed = true;
                break;
            }
            value = source->data[lexer->index];
        }
        if (!leme_scfg_text_push(&text, value)) {
            free(text.data);
            return false;
        }
        lexer->index++;
    }
    if (failed) {
        free(text.data);
        leme_scfg_skip_line(lexer);
        return true;
    }
    span = (struct leme_scfg_span){
        .offset = start,
        .length = lexer->index - start,
    };
    return leme_scfg_emit_word(lexer, &text, span);
}

static bool
leme_scfg_lex_quoted(struct leme_scfg_lexer *lexer, char quote)
{
    const struct leme_scfg_source *source = lexer->source;
    struct leme_scfg_text text = {0};
    size_t start = lexer->index;
    struct leme_scfg_span span;

    lexer->index++;
    while (lexer->index < source->length) {
        char value = source->data[lexer->index];

        if (value == '\n') {
            break;
        }
        if (value == quote) {
            lexer->index++;
            span = (struct leme_scfg_span){
                .offset = start,
                .length = lexer->index - start,
            };
            return leme_scfg_emit_word(lexer, &text, span);
        }
        if (value == '\\' && quote == '"') {
            lexer->index++;
            if (lexer->index >= source->length ||
                    source->data[lexer->index] == '\n') {
                break;
            }
            value = source->data[lexer->index];
        }
        if (!leme_scfg_text_push(&text, value)) {
            free(text.data);
            return false;
        }
        lexer->index++;
    }
    span = (struct leme_scfg_span){.offset = start, .length = 1};
    leme_scfg_error_add(lexer->result, span, quote == '"' ?
        "unterminated double-quoted string" :
        "unterminated single-quoted string");
    free(text.data);
    leme_scfg_skip_line(lexer);
    return true;
}

bool
leme_scfg_lex(const struct leme_scfg_source *source,
    struct leme_scfg_tokens *tokens, struct leme_scfg_result *result)
{
    struct leme_scfg_lexer lexer = {
        .source = source,
        .tokens = tokens,
        .result = result,
    };
    struct leme_scfg_span span;

    while (lexer.index < source->length) {
        char value = source->data[lexer.index];

        if (value == ' ' || value == '\t') {
            lexer.index++;
            continue;
        }
        if (leme_scfg_is_comment(source, lexer.index)) {
            leme_scfg_skip_line(&lexer);
            continue;
        }
        span = (struct leme_scfg_span){.offset = lexer.index, .length = 1};
        /*
         * O ';' termina uma directiva tal como a mudança de linha. Sem ele,
         * { width 0.5; height 0.6 } seria uma só directiva com três
         * parâmetros, e um bloco numa linha não serviria para nada.
         */
        if (value == '\n' || value == ';' || value == '{' || value == '}') {
            enum leme_scfg_token_kind kind =
                value == '\n' || value == ';' ?
                LEME_SCFG_TOKEN_NEWLINE : value == '{' ?
                LEME_SCFG_TOKEN_OPEN : LEME_SCFG_TOKEN_CLOSE;

            lexer.index++;
            if (!leme_scfg_token_push(tokens, kind, NULL, span)) {
                return false;
            }
            continue;
        }
        if (value == '"' || value == '\'') {
            if (!leme_scfg_lex_quoted(&lexer, value)) {
                return false;
            }
            continue;
        }
        if (!leme_scfg_lex_atom(&lexer)) {
            return false;
        }
    }
    span = (struct leme_scfg_span){.offset = source->length, .length = 0};
    if (!leme_scfg_token_push(tokens, LEME_SCFG_TOKEN_END, NULL, span)) {
        return false;
    }
    return result->error_count == 0;
}

void
leme_scfg_tokens_finish(struct leme_scfg_tokens *tokens)
{
    size_t index;

    for (index = 0; index < tokens->count; index++) {
        free(tokens->entries[index].value);
    }
    free(tokens->entries);
    *tokens = (struct leme_scfg_tokens){0};
}

static void
leme_scfg_block_finish(struct leme_scfg_block *block)
{
    size_t index;

    for (index = 0; index < block->directives_len; index++) {
        struct leme_scfg_directive *directive = &block->directives[index];
        size_t param;

        free(directive->name);
        for (param = 0; param < directive->params_len; param++) {
            free(directive->params[param]);
        }
        free(directive->params);
        free(directive->param_spans);
        leme_scfg_block_finish(&directive->children);
    }
    free(block->directives);
    *block = (struct leme_scfg_block){0};
}

void
leme_scfg_result_finish(struct leme_scfg_result *result)
{
    size_t index;

    for (index = 0; index < result->error_count; index++) {
        free(result->errors[index].message);
    }
    free(result->errors);
    leme_scfg_block_finish(&result->block);
    *result = (struct leme_scfg_result){0};
}
