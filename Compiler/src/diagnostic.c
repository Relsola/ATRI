#include "frontend.h"

#include <string.h>

AtriLocation atri_source_location(AtriSource source, size_t offset) {
    if (offset > source.length) {
        offset = source.length;
    }
    AtriLocation location = {.line = 1, .column = 1};
    size_t cursor = 0;
    if (source.length >= 3 && memcmp(source.text, "\xEF\xBB\xBF", 3) == 0 &&
        offset >= 3) {
        cursor = 3;
        location.line_start = 3;
    }
    /* 列数按 UTF-8 字符起始字节递增，不把续字节另算一列；不等于终端中的实际显示宽度。 */
    while (cursor < offset) {
        unsigned char c = (unsigned char)source.text[cursor++];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && cursor < source.length &&
                source.text[cursor] == '\n' && cursor < offset) {
                ++cursor;
            }
            ++location.line;
            location.column = 1;
            location.line_start = cursor;
        } else if ((c & 0xC0) != 0x80) {
            ++location.column;
        }
    }
    location.line_end = location.line_start;
    while (location.line_end < source.length &&
           source.text[location.line_end] != '\n' &&
           source.text[location.line_end] != '\r') {
        ++location.line_end;
    }
    return location;
}

void atri_print_diagnostic(FILE *stream, AtriSource source,
                           AtriDiagnostic diagnostic) {
    AtriLocation location = atri_source_location(source, diagnostic.span.start);
    fprintf(stream, "%s:%zu:%zu: error: %s\n",
            source.name, location.line, location.column, diagnostic.message);
    fwrite(source.text + location.line_start, 1,
           location.line_end - location.line_start, stream);
    fputc('\n', stream);
    for (size_t i = location.line_start;
         i < diagnostic.span.start && i < location.line_end; ++i) {
        unsigned char c = (unsigned char)source.text[i];
        if ((c & 0xC0) != 0x80) {
            fputc(c == '\t' ? '\t' : ' ', stream);
        }
    }
    fputs("^\n", stream);
}
