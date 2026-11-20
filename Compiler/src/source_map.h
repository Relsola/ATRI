#ifndef ATRI_SOURCE_MAP_H
#define ATRI_SOURCE_MAP_H
#include "frontend.h"

typedef struct AtriSourceMap AtriSourceMap;
bool atri_source_edit(AtriSource *source, AtriSourceMap **map, AtriSpan replaced, AtriSpan origin,
                      const char *text, size_t size, const char *producer);
void atri_source_map_destroy(AtriSourceMap *map);
void atri_source_label(FILE *stream, const AtriSourceMap *map, AtriSource source, AtriSpan span,
                       const char *level, const char *message, const char *detail);
#endif
