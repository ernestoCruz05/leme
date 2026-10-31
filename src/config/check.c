#include "config/config.h"
#include "config/render.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int leme_config_check(const char *path, FILE *stream) {
  struct leme_config *config;
  char *error = NULL;
  size_t index;
  size_t length;
  size_t errors = 0;
  size_t warnings = 0;
  bool colour;
  bool valid;

  if (path == NULL) {
    fprintf(stream, "no configuration path is available\n");
    return 1;
  }
  config = leme_config_load(path, &error);
  if (config == NULL) {
    if (error == NULL) {
      fprintf(stream, "%s: the configuration could not be read\n", path);
    } else {
      length = strlen(error);
      fprintf(stream, "%s%s", error,
              length > 0 && error[length - 1] == '\n' ? "" : "\n");
    }
    free(error);
    fprintf(stream, "%s: configuration rejected\n", path);
    return 1;
  }
  free(error);
  error = NULL;
  valid = leme_config_validate(config, &error);
  if (valid && config->diagnostics.count == 0) {
    fprintf(stream, "%s: no problems found\n", path);
    leme_config_destroy(config);
    return 0;
  }
  colour = isatty(fileno(stream));
  for (index = 0; index < config->diagnostics.count; index++) {
    char *rendered = leme_diagnostic_render_rich(
        config, &config->diagnostics.entries[index], colour);

    if (rendered == NULL) {
      fprintf(stream, "%s: out of memory while reporting\n", path);
      free(error);
      leme_config_destroy(config);
      return 1;
    }
    fputs(rendered, stream);
    fputc('\n', stream);
    free(rendered);
    if (config->diagnostics.entries[index].severity == LEME_DIAGNOSTIC_ERROR) {
      errors++;
    } else {
      warnings++;
    }
  }
  if (warnings > 0 && errors > 0) {
    fprintf(stream, "%s: %zu %s, %zu %s\n", path, warnings,
            warnings == 1 ? "warning" : "warnings", errors,
            errors == 1 ? "error" : "errors");
  } else if (warnings > 0) {
    fprintf(stream, "%s: %zu %s\n", path, warnings,
            warnings == 1 ? "warning" : "warnings");
  } else if (errors > 0) {
    fprintf(stream, "%s: %zu %s\n", path, errors,
            errors == 1 ? "error" : "errors");
  }
  if (config->diagnostics.truncated) {
    fprintf(stream, "%s: further problems were not recorded\n", path);
  }
  if (!valid) {
    fprintf(stream, "%s\n%s: configuration rejected\n",
            error != NULL ? error : "invalid configuration", path);
  }
  free(error);
  leme_config_destroy(config);
  return 1;
}
