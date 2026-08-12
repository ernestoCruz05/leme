#include "config/scfg.h"

#include <stdio.h>
#include <stdlib.h>

static bool
leme_scfg_source_index(struct leme_scfg_source *source)
{
    size_t index;
    size_t count = 1;
    size_t *offsets;
    size_t next = 1;

    for (index = 0; index < source->length; index++) {
        if (source->data[index] == '\n') {
            count++;
        }
    }
    offsets = calloc(count, sizeof(*offsets));
    if (offsets == NULL) {
        return false;
    }
    offsets[0] = 0;
    for (index = 0; index < source->length && next < count; index++) {
        if (source->data[index] == '\n') {
            offsets[next++] = index + 1;
        }
    }
    source->line_offsets = offsets;
    source->line_count = count;
    return true;
}

bool
leme_scfg_source_load(struct leme_scfg_source *source, const char *path)
{
    FILE *file = fopen(path, "r");
    long size;

    *source = (struct leme_scfg_source){0};
    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    source->data = malloc((size_t)size + 1);
    if (source->data == NULL) {
        fclose(file);
        return false;
    }
    source->length = fread(source->data, 1, (size_t)size, file);
    if (ferror(file) != 0) {
        fclose(file);
        leme_scfg_source_finish(source);
        return false;
    }
    source->data[source->length] = '\0';
    fclose(file);
    if (!leme_scfg_source_index(source)) {
        leme_scfg_source_finish(source);
        return false;
    }
    return true;
}

void
leme_scfg_source_finish(struct leme_scfg_source *source)
{
    free(source->data);
    free(source->line_offsets);
    *source = (struct leme_scfg_source){0};
}

void
leme_scfg_source_locate(const struct leme_scfg_source *source,
    struct leme_scfg_span span, int *line, int *column)
{
    size_t low = 0;
    size_t high = source->line_count;

    while (low + 1 < high) {
        size_t middle = low + (high - low) / 2;

        if (source->line_offsets[middle] <= span.offset) {
            low = middle;
        } else {
            high = middle;
        }
    }
    *line = (int)low + 1;
    *column = (int)(span.offset - source->line_offsets[low]) + 1;
}

bool
leme_scfg_source_line_text(const struct leme_scfg_source *source,
    int line, const char **text, size_t *length)
{
    size_t start;
    size_t end;

    if (line < 1 || (size_t)line > source->line_count) {
        return false;
    }
    start = source->line_offsets[line - 1];
    end = start;
    while (end < source->length && source->data[end] != '\n') {
        end++;
    }
    *text = source->data + start;
    *length = end - start;
    return true;
}
