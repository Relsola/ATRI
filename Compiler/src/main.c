#include "frontend.h"
#include "ir.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static char *read_all(FILE *stream, size_t *length) {
    size_t capacity = 4096;
    char *text = malloc(capacity + 1);
    if (text == NULL) {
        return NULL;
    }
    *length = 0;
    for (;;) {
        if (*length == capacity) {
            if (capacity > (SIZE_MAX - 1) / 2) {
                free(text);
                return NULL;
            }
            capacity *= 2;
            char *grown = realloc(text, capacity + 1);
            if (grown == NULL) {
                free(text);
                return NULL;
            }
            text = grown;
        }
        size_t count = fread(text + *length, 1, capacity - *length, stream);
        *length += count;
        if (count == 0) {
            if (ferror(stream)) {
                free(text);
                return NULL;
            }
            break;
        }
    }
    text[*length] = '\0';
    return text;
}

static void usage(FILE *stream, const char *name) {
    fprintf(stream, "Usage: %s [--emit-ast|--emit-ir|--emit-ir-before-opt|-S] [-O0|-O1] [-o output] <source-file|->\n", name);
    fputs("-O1 (default): fold constants and inline small nonrecursive leaf functions.\n"
          "-O0: fold local constants, preserve function calls.\n"
          "Assembly targets Windows x64 with clang's integrated assembler.\n", stream);
}

int main(int argc, char **argv) {
    enum { EMIT_AST, EMIT_IR, EMIT_RAW_IR, EMIT_ASM } mode;
#ifdef ATRI_COMPILER_DRIVER
    mode = EMIT_ASM;
#else
    mode = EMIT_AST;
#endif
    bool inline_functions = true;
    const char *input = NULL, *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout, argv[0]); return 0; }
        if (strcmp(argv[i], "--emit-ast") == 0) mode = EMIT_AST;
        else if (strcmp(argv[i], "--emit-ir") == 0) mode = EMIT_IR;
        else if (strcmp(argv[i], "--emit-ir-before-opt") == 0) mode = EMIT_RAW_IR;
        else if (strcmp(argv[i], "-S") == 0) mode = EMIT_ASM;
        else if (strcmp(argv[i], "-O0") == 0) inline_functions = false;
        else if (strcmp(argv[i], "-O1") == 0) inline_functions = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (input == NULL && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0)) input = argv[i];
        else { usage(stderr, argv[0]); return 2; }
    }
    if (input == NULL || (output != NULL && strcmp(input, output) == 0)) {
        usage(stderr, argv[0]); return 2;
    }
    bool from_stdin = strcmp(input, "-") == 0;
#ifdef _WIN32
    if (from_stdin && _setmode(_fileno(stdin), _O_BINARY) == -1) {
        fputs("<stdin>: error: cannot enable binary input\n", stderr);
        return 2;
    }
#endif
    FILE *stream = from_stdin ? stdin : fopen(input, "rb");
    if (stream == NULL) {
        fprintf(stderr, "%s: error: cannot open source file\n", input);
        return 2;
    }
    size_t length = 0;
    char *text = read_all(stream, &length);
    if (!from_stdin) {
        fclose(stream);
    }
    if (text == NULL) {
        fprintf(stderr, "%s: error: cannot read source or allocate memory\n", input);
        return 2;
    }

    AtriSource source = {
        .name = from_stdin ? "<stdin>" : input,
        .text = text,
        .length = length
    };
    AtriProgram program;
    AtriDiagnostic diagnostic;
    if (!atri_parse(source, &program, &diagnostic)) {
        atri_print_diagnostic(stderr, source, diagnostic);
        free(text);
        return 1;
    }
    AtriIRProgram ir = {0};
    if (mode != EMIT_AST && (!atri_lower(source, &program, &ir, &diagnostic) ||
        (mode != EMIT_RAW_IR && !atri_optimize(&ir, inline_functions, &diagnostic)))) {
        atri_print_diagnostic(stderr, source, diagnostic);
        atri_ir_destroy(&ir); atri_program_destroy(&program); free(text); return 1;
    }
    FILE *destination = output == NULL ? stdout : fopen(output, "wb");
    if (destination == NULL) {
        fprintf(stderr, "%s: error: cannot open output file\n", output);
        atri_ir_destroy(&ir); atri_program_destroy(&program); free(text); return 2;
    }
    bool success = true;
    if (mode == EMIT_AST) atri_dump_ast(destination, source, &program);
    else if (mode == EMIT_IR || mode == EMIT_RAW_IR) atri_dump_ir(destination, source, &ir);
    else success = atri_emit_x64(destination, source, &ir, &diagnostic);
    bool io_error = fflush(destination) == EOF || ferror(destination);
    if (output != NULL && fclose(destination) == EOF) io_error = true;
    if (!success) atri_print_diagnostic(stderr, source, diagnostic);
    if (io_error) fprintf(stderr, "%s: error: cannot write output\n", output == NULL ? "<stdout>" : output);
    atri_ir_destroy(&ir);
    atri_program_destroy(&program);
    free(text);
    return io_error ? 2 : success ? 0 : 1;
}
