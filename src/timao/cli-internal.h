#ifndef TIMAO_CLI_INTERNAL_H
#define TIMAO_CLI_INTERNAL_H
#include "timao/runtime-internal.h"

enum timao_cli_kind {
  TIMAO_CLI_HELP,
  TIMAO_CLI_VERSION,
  TIMAO_CLI_EVAL,
  TIMAO_CLI_RUN,
  TIMAO_CLI_REPL,
  TIMAO_CLI_COMMAND,
  TIMAO_CLI_GET,
  TIMAO_CLI_WATCH
};
#define TIMAO_CLI_FIELD_MAX 64
#define TIMAO_CLI_FIELDS_MAX 16

struct timao_cli_arguments {
  enum timao_cli_kind kind;
  enum timao_output_mode format;
  const char *socket, *source, *command;
  char *const *arguments;
  size_t count;
};
bool timao_cli_diagnostic(const struct timao_diagnostic *error,
                          enum timao_output_mode format, char *buffer,
                          size_t capacity, struct timao_output_buffer *out);
struct timao_cli_reporter {
  struct timao_output_writer *writer;
  char *buffer;
  size_t capacity;
};
bool timao_cli_report(struct timao_runtime *runtime,
                      struct timao_output_writer *writer,
                      const struct timao_diagnostic *error, char *buffer,
                      size_t capacity);
int timao_cli_repl(struct timao_runtime *runtime, int input_fd,
                   const struct timao_cli_reporter *reporter,
                   struct timao_diagnostic *error);
struct timao_cli_source {
  char *data;
  size_t length;
};
int timao_cli_source_load(struct timao_runtime *runtime, const char *path,
                          int input_fd, struct timao_cli_source *out,
                          struct timao_diagnostic *error);
void timao_cli_source_destroy(struct timao_cli_source *source);
int timao_cli_parse(int argc, char *const *argv,
                    struct timao_cli_arguments *out,
                    struct timao_diagnostic *error);
#endif
