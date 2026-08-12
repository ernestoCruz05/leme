#ifndef LEME_H
#define LEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LEME_ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct leme_box {
    int x;
    int y;
    int width;
    int height;
};

#endif
