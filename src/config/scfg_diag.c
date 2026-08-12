#include "config/scfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *
leme_scfg_render_error(const struct leme_scfg_source *source,
    const char *path, const struct leme_scfg_error *error)
{
    const char *line_text = "";
    size_t line_length = 0;
    size_t padding_length;
    size_t offset;
    size_t total;
    size_t index;
    char *rendered;
    int header;
    int line = 0;
    int column = 0;

    leme_scfg_source_locate(source, error->span, &line, &column);
    if (!leme_scfg_source_line_text(source, line, &line_text, &line_length)) {
        line_text = "";
        line_length = 0;
        column = 1;
    }
    padding_length = (size_t)(column - 1);
    if (padding_length > line_length) {
        padding_length = line_length;
    }
    header = snprintf(NULL, 0, "%s:%d:%d: %s\n", path, line, column,
        error->message);
    if (header < 0) {
        return NULL;
    }
    total = (size_t)header + line_length + padding_length + 4;
    rendered = malloc(total);
    if (rendered == NULL) {
        return NULL;
    }
    header = snprintf(rendered, total, "%s:%d:%d: %s\n", path, line, column,
        error->message);
    if (header < 0) {
        free(rendered);
        return NULL;
    }
    offset = (size_t)header;
    memcpy(rendered + offset, line_text, line_length);
    offset += line_length;
    rendered[offset++] = '\n';
    /*
     * O avanço copia o tabulador tal como está. Substituí-lo por um espaço
     * desalinhava o acento circunflexo em qualquer terminal.
     */
    for (index = 0; index < padding_length; index++) {
        rendered[offset++] = line_text[index] == '\t' ? '\t' : ' ';
    }
    rendered[offset++] = '^';
    rendered[offset++] = '\n';
    rendered[offset] = '\0';
    return rendered;
}
