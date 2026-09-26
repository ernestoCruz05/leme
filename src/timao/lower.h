#ifndef TIMAO_LOWER_H
#define TIMAO_LOWER_H

#include "timao/language.h"
#include "timao/source.h"

struct timao_lowered;
enum timao_boundary { TIMAO_QUERY, TIMAO_ACT, TIMAO_WATCH_BOUNDARY };
enum timao_status timao_lower(struct timao_execution *execution,
                              const struct timao_program *program,
                              uint32_t node, enum timao_boundary boundary,
                              struct timao_lowered **out,
                              struct timao_diagnostic *error);
const struct leme_public_value *
timao_lowered_value(const struct timao_lowered *lowered);
struct timao_source *timao_lowered_source(const struct timao_lowered *lowered);
bool timao_lowered_span(const struct timao_lowered *lowered,
                        struct leme_public_text expr_path,
                        struct timao_span *out);
void timao_lowered_ref(const struct timao_lowered *lowered);
void timao_lowered_unref(const struct timao_lowered *lowered);

#endif
