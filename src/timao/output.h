#ifndef TIMAO_OUTPUT_H
#define TIMAO_OUTPUT_H

#include "timao/request.h"
#include "yyjson.h"

#include <stdbool.h>
#include <stdio.h>

bool timao_output_value(FILE *stream, enum timao_format format,
                        yyjson_val *value);

#endif
