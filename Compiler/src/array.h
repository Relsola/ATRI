#ifndef ATRI_ARRAY_H
#define ATRI_ARRAY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline bool atri_append(void **data, size_t *count, size_t *capacity,
                               size_t width, const void *element) {
    if (*count == SIZE_MAX || *count >= SIZE_MAX / width) return false;
    if (*count == *capacity) {
        size_t next = *capacity == 0 ? 8 : *capacity;
        if (next <= SIZE_MAX / width / 2) next *= 2;
        else next = *count + 1;
        void *grown = realloc(*data, next * width);
        if (grown == NULL) return false;
        *data = grown;
        *capacity = next;
    }
    memcpy((char *)*data + *count * width, element, width);
    ++*count;
    return true;
}
#endif
