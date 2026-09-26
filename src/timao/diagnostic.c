#include "timao/diagnostic.h"
#include "timao/source.h"
#include "timao/language-internal.h"
#include "timao/lower.h"
#include "public/value-internal.h"
#include <string.h>

void timao_diagnostic_init(struct timao_diagnostic *error) {
  if (error != NULL)
    *error = (struct timao_diagnostic){0};
}
void timao_diagnostic_destroy(struct timao_diagnostic *error) {
  if (error == NULL)
    return;
  timao_source_unref(error->source);
  leme_public_builder_destroy(error->payload_owner);
  if (error->release_owner != NULL)
    error->release_owner(error->retained_owner);
  for (size_t i = 0; i < error->call_count; ++i)
    timao_source_unref(error->calls[i].source);
  *error = (struct timao_diagnostic){0};
}
void timao_diagnostic_at(struct timao_diagnostic *error,
                         struct timao_source *source, struct timao_span span) {
  if (error == NULL || source == NULL)
    return;
  timao_source_ref(source);
  timao_source_unref(error->source);
  error->source = source;
  error->span = span;
  timao_source_position(source, span.begin, &error->line, &error->column);
}
void timao_diagnostic_call(struct timao_diagnostic *error,
                           struct timao_source *source, struct timao_span span,
                           struct timao_span name) {
  if (error == NULL || source == NULL)
    return;
  if (error->call_count == sizeof(error->calls) / sizeof(error->calls[0])) {
    error->calls_truncated = true;
    return;
  }
  timao_source_ref(source);
  error->calls[error->call_count++] =
      (struct timao_call_site){.source = source, .span = span, .name = name};
}
static void store_coordinate(size_t *output, size_t value) {
  if (output != NULL)
    *output = value;
}
void timao_diagnostic_position(const struct timao_diagnostic *error,
                               size_t *line, size_t *column) {
  store_coordinate(line, error == NULL || error->line == 0 ? 1 : error->line);
  store_coordinate(column,
                   error == NULL || error->column == 0 ? 1 : error->column);
}
struct leme_public_text
timao_diagnostic_name(const struct timao_diagnostic *error) {
  if (error == NULL)
    return LEME_PUBLIC_TEXT("<unknown>");
  if (error->source == NULL)
    return error->fallback_name_length == 0
               ? LEME_PUBLIC_TEXT("<unknown>")
               : (struct leme_public_text){error->fallback_name,
                                           error->fallback_name_length};
  if (error->source->input.name.length != 0)
    return error->source->input.name;
  return error->source->input.mode == TIMAO_REPL ? LEME_PUBLIC_TEXT("<repl>")
         : error->source->input.mode == TIMAO_EXPRESSION
             ? LEME_PUBLIC_TEXT("<expression>")
             : LEME_PUBLIC_TEXT("<stdin>");
}
static void copy_text(char *out, size_t capacity, const char *text) {
  size_t length = 0;
  while (length + 1 < capacity && text[length] != '\0') {
    out[length] = text[length];
    ++length;
  }
  out[length] = '\0';
}
static void copy_public_text(char *out, size_t capacity,
                             struct leme_public_text text) {
  const size_t length = text.length < capacity - 1 ? text.length : capacity - 1;
  if (length != 0)
    memcpy(out, text.data, length);
  out[length] = '\0';
}
void timao_diagnostic_input(struct timao_diagnostic *error,
                            const struct timao_input *input) {
  if (error == NULL || input == NULL || error->source != NULL)
    return;
  struct leme_public_text name = input->name;
  if (name.length == 0 || name.data == NULL)
    name = input->mode == TIMAO_REPL         ? LEME_PUBLIC_TEXT("<repl>")
           : input->mode == TIMAO_EXPRESSION ? LEME_PUBLIC_TEXT("<expression>")
                                             : LEME_PUBLIC_TEXT("<stdin>");
  copy_public_text(error->fallback_name, sizeof(error->fallback_name), name);
  error->fallback_name_length = name.length < sizeof(error->fallback_name) - 1
                                    ? name.length
                                    : sizeof(error->fallback_name) - 1;
  error->line = 1;
  error->column = 1;
  const size_t offset = error->span.begin < input->bytes.length
                            ? error->span.begin
                            : input->bytes.length;
  if (input->bytes.data == NULL)
    return;
  for (size_t i = 0; i < offset; ++i) {
    const unsigned char byte = (unsigned char)input->bytes.data[i];
    if (byte == '\n') {
      ++error->line;
      error->column = 1;
    } else if ((byte & 0xc0u) != 0x80u)
      ++error->column;
  }
}
static enum timao_status
describe_remote(const struct leme_public_value *payload,
                const struct timao_lowered *descriptor,
                struct timao_diagnostic *error);

enum timao_status
timao_diagnostic_take_owned(void *owner, void (*release)(void *),
                            const struct leme_public_value *payload,
                            const struct timao_lowered *descriptor,
                            struct timao_diagnostic *error) {
  if (error != NULL && error->retained_owner == owner &&
      error->release_owner == release) {
    error->retained_owner = NULL;
    error->release_owner = NULL;
  }
  if (owner == NULL || release == NULL || payload == NULL ||
      !payload->owner->sealed ||
      leme_public_kind(payload) != LEME_PUBLIC_OBJECT) {
    if (release != NULL && owner != NULL)
      release(owner);
    return timao_error(error, "invalid_argument",
                       "owned diagnostic requires a sealed object");
  }
  if (error == NULL) {
    release(owner);
    return TIMAO_ERROR;
  }
  timao_diagnostic_destroy(error);
  error->retained_owner = owner;
  error->release_owner = release;
  error->payload = payload;
  return describe_remote(payload, descriptor, error);
}

enum timao_status timao_diagnostic_take_remote(
    struct leme_public_builder *owner, const struct leme_public_value *payload,
    const struct timao_lowered *descriptor, struct timao_diagnostic *error) {
  if (error != NULL && error->payload_owner == owner)
    error->payload_owner = NULL;
  if (owner == NULL || payload == NULL || payload->owner != owner ||
      !owner->sealed || leme_public_kind(payload) != LEME_PUBLIC_OBJECT) {
    leme_public_builder_destroy(owner);
    return timao_error(error, "invalid_argument",
                       "remote diagnostic requires an owned sealed object");
  }
  if (error == NULL) {
    leme_public_builder_destroy(owner);
    return TIMAO_ERROR;
  }
  timao_diagnostic_destroy(error);
  leme_public_builder_set_work(owner, NULL);
  error->payload_owner = owner;
  error->payload = payload;
  return describe_remote(payload, descriptor, error);
}

static enum timao_status
describe_remote(const struct leme_public_value *payload,
                const struct timao_lowered *descriptor,
                struct timao_diagnostic *error) {
  struct leme_public_text code = LEME_PUBLIC_TEXT("remote_error"),
                          message = LEME_PUBLIC_TEXT("remote request failed"),
                          path = {0};
  if (leme_public_as_text(leme_public_get(payload, LEME_PUBLIC_TEXT("code")),
                          &code) != LEME_PUBLIC_OK)
    code = LEME_PUBLIC_TEXT("remote_error");
  if (leme_public_as_text(leme_public_get(payload, LEME_PUBLIC_TEXT("message")),
                          &message) != LEME_PUBLIC_OK)
    message = LEME_PUBLIC_TEXT("remote request failed");
  copy_public_text(error->code, sizeof(error->code), code);
  copy_public_text(error->message, sizeof(error->message), message);
  struct timao_span span = {0};
  if (timao_lowered_span(descriptor, LEME_PUBLIC_TEXT("/expr"), &span)) {
    if (leme_public_as_text(
            leme_public_get(payload, LEME_PUBLIC_TEXT("expr_path")), &path) ==
        LEME_PUBLIC_OK) {
      struct timao_span precise = {0};
      if (timao_lowered_span(descriptor, path, &precise))
        span = precise;
    }
    timao_diagnostic_at(error, timao_lowered_source(descriptor), span);
  }
  return TIMAO_ERROR;
}
enum timao_status
timao_diagnostic_value(struct timao_execution *execution,
                       const struct timao_diagnostic *diagnostic,
                       const struct timao_value **out,
                       struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument",
                       "missing diagnostic value output");
  *out = NULL;
  if (execution == NULL || execution->vm == NULL || diagnostic == NULL ||
      diagnostic == error)
    return timao_error(error, "invalid_argument",
                       "invalid diagnostic conversion");
  if (diagnostic->payload != NULL)
    return timao_import(execution, diagnostic->payload, out, error);
  const struct timao_value *code = NULL, *message = NULL;
  if (timao_value_string(
          execution->vm,
          (struct leme_public_text){diagnostic->code, strlen(diagnostic->code)},
          &code, error) != TIMAO_OK ||
      timao_value_string(execution->vm,
                         (struct leme_public_text){diagnostic->message,
                                                   strlen(diagnostic->message)},
                         &message, error) != TIMAO_OK)
    return TIMAO_ERROR;
  const struct timao_member members[] = {
      {.key = LEME_PUBLIC_TEXT("code"), .value = code},
      {.key = LEME_PUBLIC_TEXT("message"), .value = message}};
  return timao_value_object(execution->vm, members, 2, out, error);
}
enum timao_status timao_error(struct timao_diagnostic *error, const char *code,
                              const char *message) {
  if (error != NULL) {
    char saved_code[48] = {0}, saved_message[192] = {0};
    copy_text(saved_code, sizeof(saved_code), code);
    copy_text(saved_message, sizeof(saved_message), message);
    timao_diagnostic_destroy(error);
    memcpy(error->code, saved_code, sizeof(saved_code));
    memcpy(error->message, saved_message, sizeof(saved_message));
  }
  return TIMAO_ERROR;
}
