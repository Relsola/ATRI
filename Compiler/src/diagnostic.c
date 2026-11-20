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

void atri_diagnostic_note(AtriDiagnostic *d, AtriSpan span, const char *message) {
    if (d->note_count == sizeof(d->notes) / sizeof(*d->notes)) {
        ++d->omitted;
        return;
    }
    d->notes[d->note_count++] = (AtriDiagnosticLabel){span, message};
}

void atri_print_label(FILE *stream, AtriSource source, AtriSpan span, const char *level,
                      const char *message, const char *detail) {
    AtriLocation location = atri_source_location(source, span.start);
    fprintf(stream, "%s:%zu:%zu: %s: %s%s%s\n", source.name, location.line, location.column,
            level, message, *detail ? ": " : "", detail);
    fwrite(source.text + location.line_start, 1,
           location.line_end - location.line_start, stream);
    fputc('\n', stream);
    for (size_t i = location.line_start;
         i < span.start && i < location.line_end; ++i) {
        unsigned char c = (unsigned char)source.text[i];
        if ((c & 0xC0) != 0x80) {
            fputc(c == '\t' ? '\t' : ' ', stream);
        }
    }
    fputc('^', stream);
    for (size_t i = span.start; i < span.end && i < location.line_end; ++i) {
        if (i != span.start && ((unsigned char)source.text[i] & 0xC0) != 0x80) {
            fputc('~', stream);
        }
    }
    fputc('\n', stream);
}

void atri_print_diagnostic(FILE *stream, AtriSource source, AtriDiagnostic diagnostic) {
    atri_program_diagnostic(stream, source, &(AtriProgram){0}, diagnostic);
}
