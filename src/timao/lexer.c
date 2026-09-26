#include "timao/lexer.h"
#include "timao/diagnostic.h"
#include "control/number.h"
#include "yyjson.h"

#include <string.h>

static bool whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static bool delimiter(char c) {
  return whitespace(c) || c == '(' || c == ')' || c == ';';
}
static bool name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool ordinary_name(struct leme_public_text text) {
  if (text.length == 0 || !name_start(text.data[0]))
    return false;
  for (size_t i = 1; i < text.length; ++i)
    if (!name_start(text.data[i]) &&
        !(text.data[i] >= '0' && text.data[i] <= '9') && text.data[i] != '-')
      return false;
  return true;
}
static bool text_is(struct leme_public_text text, const char *word) {
  const size_t size = strlen(word);
  return size == text.length && memcmp(text.data, word, size) == 0;
}
static bool symbolic_name(struct leme_public_text text) {
  const char *const operators[] = {
      "+", "-", "*", "/", "=", "!=", "<", "<=", ">", ">=", "->"};
  for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i)
    if (text_is(text, operators[i]))
      return true;
  return false;
}
bool timao_name_valid(struct leme_public_text text) {
  return ordinary_name(text) || symbolic_name(text);
}
static enum timao_status checked_name(struct timao_lexer *lexer,
                                      struct leme_public_text text,
                                      bool *valid) {
  *valid = false;
  if (text.length == 0 || !name_start(text.data[0]))
    return TIMAO_OK;
  for (size_t i = 1; i < text.length; ++i) {
    if (i % 128 == 0 &&
        timao_source_check(lexer->source, lexer->error) != TIMAO_OK)
      return TIMAO_ERROR;
    const char c = text.data[i];
    if (!name_start(c) && !(c >= '0' && c <= '9') && c != '-')
      return TIMAO_OK;
  }
  *valid = true;
  return TIMAO_OK;
}

static enum timao_status fail(struct timao_lexer *lexer, const char *message,
                              size_t start) {
  timao_error(lexer->error, "syntax_error", message);
  if (lexer->error != NULL)
    lexer->error->span = (struct timao_span){start, lexer->offset};
  return TIMAO_ERROR;
}
static bool advance(struct timao_lexer *lexer) {
  if (lexer->offset % 128 == 0 &&
      timao_source_check(lexer->source, lexer->error) != TIMAO_OK)
    return false;
  ++lexer->offset;
  return true;
}
static void *allocate(void *context, size_t size) {
  struct timao_lexer *lexer = context;
  void *memory = timao_memory_alloc(lexer->source->account, size, lexer->error);
  if (memory == NULL)
    lexer->allocation_failed = true;
  return memory;
}
static void release(void *context, void *allocation) {
  (void)context;
  timao_memory_free(allocation);
}
static void *resize(void *context, void *ptr, size_t old_size, size_t size) {
  void *replacement = allocate(context, size);
  if (replacement == NULL)
    return NULL;
  struct timao_lexer *lexer = context;
  const size_t length = ptr == NULL ? 0 : old_size < size ? old_size : size;
  for (size_t offset = 0; offset < length;) {
    if (timao_source_check(lexer->source, lexer->error) != TIMAO_OK) {
      lexer->allocation_failed = true;
      release(context, replacement);
      return NULL;
    }
    const size_t count = length - offset > 128 ? 128 : length - offset;
    memcpy((char *)replacement + offset, (const char *)ptr + offset, count);
    offset += count;
  }
  release(context, ptr);
  return replacement;
}
static enum leme_public_status number_work(void *context, size_t units) {
  (void)units;
  struct timao_lexer *lexer = context;
  return timao_source_check(lexer->source, lexer->error) == TIMAO_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

static enum timao_status string_token(struct timao_lexer *lexer,
                                      struct timao_token *token) {
  const struct leme_public_text input = lexer->source->input.bytes;
  const size_t start = lexer->offset;
  if (!advance(lexer))
    return TIMAO_ERROR;
  bool escaped = false, closed = false;
  while (lexer->offset < input.length) {
    const char c = input.data[lexer->offset];
    if ((unsigned char)c < 0x20)
      return fail(lexer, "raw control in string", start);
    if (!advance(lexer))
      return TIMAO_ERROR;
    if (escaped)
      escaped = false;
    else if (c == '\\')
      escaped = true;
    else if (c == '"') {
      closed = true;
      break;
    }
  }
  if (!closed) {
    if (lexer->source->input.mode == TIMAO_REPL)
      return TIMAO_INCOMPLETE;
    return fail(lexer, "unterminated string", start);
  }
  if (lexer->offset < input.length && !delimiter(input.data[lexer->offset]))
    return fail(lexer, "missing token separator", start);
  const size_t size = lexer->offset - start;
  char *encoded = allocate(lexer, size);
  if (encoded == NULL)
    return TIMAO_ERROR;
  for (size_t offset = 0; offset < size;) {
    if (timao_source_check(lexer->source, lexer->error) != TIMAO_OK) {
      release(lexer, encoded);
      return TIMAO_ERROR;
    }
    const size_t count = size - offset > 128 ? 128 : size - offset;
    memcpy(encoded + offset, input.data + start + offset, count);
    offset += count;
  }
  yyjson_alc allocator = {
      .malloc = allocate, .realloc = resize, .free = release, .ctx = lexer};
  yyjson_read_err read_error = {0};
  yyjson_doc *document =
      yyjson_read_opts(encoded, size, 0, &allocator, &read_error);
  release(lexer, encoded);
  if (lexer->allocation_failed ||
      timao_source_check(lexer->source, lexer->error) != TIMAO_OK) {
    yyjson_doc_free(document);
    return TIMAO_ERROR;
  }
  if (document == NULL) {
    if (lexer->allocation_failed)
      return TIMAO_ERROR;
    return fail(lexer, "invalid JSON string escape", start);
  }
  yyjson_val *value = yyjson_doc_get_root(document);
  const size_t length = yyjson_get_len(value);
  char *decoded = allocate(lexer, length + 1);
  if (decoded == NULL) {
    yyjson_doc_free(document);
    return TIMAO_ERROR;
  }
  const char *data = yyjson_get_str(value);
  for (size_t p = 0; p < length;) {
    if (timao_source_check(lexer->source, lexer->error) != TIMAO_OK) {
      release(lexer, decoded);
      yyjson_doc_free(document);
      return TIMAO_ERROR;
    }
    const size_t count = length - p > 128 ? 128 : length - p;
    memcpy(decoded + p, data + p, count);
    p += count;
  }
  yyjson_doc_free(document);
  *token = (struct timao_token){.kind = TIMAO_TOKEN_STRING,
                                .span = {start, lexer->offset},
                                .text = {decoded, length},
                                .owned = decoded};
  return TIMAO_OK;
}

enum timao_status timao_lexer_next(struct timao_lexer *lexer,
                                   struct timao_token *token,
                                   struct timao_diagnostic *error) {
  if (lexer == NULL || lexer->source == NULL || token == NULL)
    return timao_error(error, "invalid_argument", "invalid lexer input");
  *token = (struct timao_token){0};
  lexer->error = error;
  lexer->allocation_failed = false;
  const struct leme_public_text input = lexer->source->input.bytes;
  if (timao_source_check(lexer->source, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (lexer->offset == 0 && input.length >= 2 && input.data[0] == '#' &&
      input.data[1] == '!' && lexer->source->input.mode == TIMAO_FILE)
    while (lexer->offset < input.length && input.data[lexer->offset] != '\n')
      if (!advance(lexer))
        return TIMAO_ERROR;
  while (lexer->offset < input.length) {
    const char c = input.data[lexer->offset];
    if (whitespace(c)) {
      if (!advance(lexer))
        return TIMAO_ERROR;
    } else if (c == ';') {
      while (lexer->offset < input.length && input.data[lexer->offset] != '\n')
        if (!advance(lexer))
          return TIMAO_ERROR;
    } else
      break;
  }
  const size_t start = lexer->offset;
  if (start == input.length) {
    token->span = (struct timao_span){start, start};
    return TIMAO_OK;
  }
  const char first = input.data[start];
  if (first == '(' || first == ')') {
    if (!advance(lexer))
      return TIMAO_ERROR;
    token->kind = first == '(' ? TIMAO_TOKEN_OPEN : TIMAO_TOKEN_CLOSE;
    token->span = (struct timao_span){start, lexer->offset};
    return TIMAO_OK;
  }
  if (first == '"')
    return string_token(lexer, token);
  while (lexer->offset < input.length && !delimiter(input.data[lexer->offset]))
    if (!advance(lexer))
      return TIMAO_ERROR;
  token->span = (struct timao_span){start, lexer->offset};
  token->text =
      (struct leme_public_text){input.data + start, lexer->offset - start};
  if (first == '.') {
    size_t component = 1;
    for (size_t i = 1; i <= token->text.length; ++i) {
      if (i % 128 == 0 && timao_source_check(lexer->source, error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (i == token->text.length || token->text.data[i] == '.') {
        bool valid = false;
        if (checked_name(lexer,
                         (struct leme_public_text){token->text.data + component,
                                                   i - component},
                         &valid) != TIMAO_OK)
          return TIMAO_ERROR;
        if (!valid)
          return fail(lexer, "invalid field path", start);
        component = i + 1;
      }
    }
    token->kind = TIMAO_TOKEN_FIELD;
    return TIMAO_OK;
  }
  if ((first >= '0' && first <= '9') ||
      (first == '-' && token->text.length > 1 && token->text.data[1] >= '0' &&
       token->text.data[1] <= '9')) {
    const struct leme_public_allocator allocator = {
        .context = lexer, .allocate = allocate, .release = release};
    const struct leme_public_work work = {.context = lexer,
                                          .step = number_work};
    const enum leme_public_status status = leme_control_parse_number_checked(
        token->text, &allocator, &work, &token->number);
    if (status != LEME_PUBLIC_OK) {
      if (lexer->allocation_failed || status == LEME_PUBLIC_LIMIT)
        return TIMAO_ERROR;
      if (status == LEME_PUBLIC_OOM)
        return timao_error(error, "out_of_memory", "number conversion failed");
      return fail(lexer, "invalid or out-of-range JSON number", start);
    }
    token->kind = TIMAO_TOKEN_NUMBER;
    return TIMAO_OK;
  }
  bool valid = false;
  if (checked_name(lexer, token->text, &valid) != TIMAO_OK)
    return TIMAO_ERROR;
  if (!valid && !symbolic_name(token->text))
    return fail(lexer, "invalid identifier", start);
  token->kind = text_is(token->text, "true")    ? TIMAO_TOKEN_TRUE
                : text_is(token->text, "false") ? TIMAO_TOKEN_FALSE
                : text_is(token->text, "null")  ? TIMAO_TOKEN_NULL
                                                : TIMAO_TOKEN_NAME;
  return TIMAO_OK;
}

void timao_token_finish(struct timao_token *token) {
  if (token == NULL)
    return;
  timao_memory_free(token->owned);
  *token = (struct timao_token){0};
}
