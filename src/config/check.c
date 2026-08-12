#include "config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
leme_config_check(const char *path, FILE *stream)
{
    struct leme_config *config;
    char *error = NULL;
    size_t index;
    size_t length;

    if (path == NULL) {
        fprintf(stream, "no configuration path is available\n");
        return 1;
    }
    config = leme_config_load(path, &error);
    if (config == NULL) {
        /*
         * Os erros de sintaxe já trazem a mudança de linha final; os
         * restantes não, por isso a falta é reposta aqui.
         */
        if (error == NULL) {
            fprintf(stream, "%s: the configuration could not be read\n",
                path);
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
    if (config->diagnostics.count == 0) {
        fprintf(stream, "%s: no problems found\n", path);
        leme_config_destroy(config);
        return 0;
    }
    for (index = 0; index < config->diagnostics.count; index++) {
        fprintf(stream, "%s:%d: %s\n", path,
            config->diagnostics.entries[index].line,
            config->diagnostics.entries[index].message);
    }
    fprintf(stream, "%s: %zu %s found\n", path, config->diagnostics.count,
        config->diagnostics.count == 1 ? "problem" : "problems");
    if (config->diagnostics.truncated) {
        fprintf(stream, "%s: further problems were not recorded\n", path);
    }
    leme_config_destroy(config);
    return 1;
}
