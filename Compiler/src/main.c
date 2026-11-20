#include "frontend.h"
#include "ir.h"
#include "meta.h"
#include "meta_syntax.h"
#include "clang_driver.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void usage(FILE *stream, const char *name) {
    fprintf(stream, "Usage: %s [--emit-ast|--emit-ir|--emit-ir-before-opt|-S|-c|--exe] [-O0|-O1|-O2] [--meta plugin.ar|plugin.dll]... [-o output] <source-file|->\n", name);
    fputs("-O1: run ATRI integer simplification on LLVM IR.\n"
          "-O0 (default): translate register instructions directly to x64.\n"
          "-O2: use LLVM via clang; --emit-ir prints optimized LLVM IR.\n"
          "-S writes Windows x64 assembly. -c and --exe invoke clang to produce a COFF object or executable.\n"
          "Binary modes default to program.obj or program.exe; -o chooses the filename.\n", stream);
}

typedef enum { EMIT_AST, EMIT_IR, EMIT_RAW_IR, EMIT_ASM, EMIT_OBJECT, EMIT_EXE } EmitMode;
static int compile(EmitMode mode, unsigned level, const char *input, const char *output,
                   const AtriListener *listeners, size_t meta_count) {
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
    char *text = atri_read_stream(stream, &length);
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
    AtriProgram program = {0};
    AtriDiagnostic diagnostic;
    AtriSource loaded = source;
    bool parsed = atri_expand_program(source, listeners, meta_count, &loaded, &program, &diagnostic);
    if (loaded.text != source.text) { free(text); text = (char *)loaded.text; source = loaded; }
    if (!parsed) {
        atri_program_diagnostic(stderr, source, &program, diagnostic);
        atri_program_destroy(&program); free(text); return 1;
    }
    AtriIRProgram ir = {0};
    if (mode != EMIT_AST && (!atri_lower(source, &program, &ir, &diagnostic) ||
        (level == 0 && mode != EMIT_AST && mode != EMIT_RAW_IR && !atri_optimize(&ir, false, &diagnostic)))) {
        atri_program_diagnostic(stderr, source, &program, diagnostic);
        atri_ir_destroy(&ir); atri_program_destroy(&program); free((char *)source.text); return 1;
    }
    bool binary = mode == EMIT_OBJECT || mode == EMIT_EXE;
    FILE *destination = (binary || output == NULL) ? stdout : fopen(output, "wb");
    if (destination == NULL) {
        fprintf(stderr, "%s: error: cannot open output file\n", output);
        atri_ir_destroy(&ir); atri_program_destroy(&program); free((char *)source.text); return 2;
    }
    bool success = true;
    if (binary) success = atri_native_file(output, source, &ir, level, mode == EMIT_EXE, &diagnostic);
    else if (mode == EMIT_AST) atri_dump_ast(destination, source, &program);
    else if (level != 0 && mode != EMIT_RAW_IR) success = atri_llvm_optimize(destination, source, &ir, level, mode == EMIT_IR, &diagnostic);
    else if (mode == EMIT_IR || mode == EMIT_RAW_IR) atri_dump_ir(destination, source, &ir);
    else success = atri_emit_x64(destination, source, &ir, &diagnostic);
    bool io_error = !binary && (fflush(destination) == EOF || ferror(destination));
    if (!binary && output != NULL && fclose(destination) == EOF) io_error = true;
    if (!success) atri_program_diagnostic(stderr, source, &program, diagnostic);
    if (io_error) fprintf(stderr, "%s: error: cannot write output\n", output == NULL ? "<stdout>" : output);
    atri_ir_destroy(&ir);
    atri_program_destroy(&program);
    free((char *)source.text);
    return io_error ? 2 : success ? 0 : 1;
}

int main(int argc, char **argv) {
    EmitMode mode;
    size_t meta_count = 0;
#ifdef ATRI_COMPILER_DRIVER
    mode = EMIT_ASM;
#else
    mode = EMIT_AST;
#endif
    unsigned level = 0;
    const char *input = NULL, *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout, argv[0]); return 0; }
        if (strcmp(argv[i], "--emit-ast") == 0) mode = EMIT_AST;
        else if (strcmp(argv[i], "--emit-ir") == 0) mode = EMIT_IR;
        else if (strcmp(argv[i], "--emit-ir-before-opt") == 0) mode = EMIT_RAW_IR;
        else if (strcmp(argv[i], "-S") == 0) mode = EMIT_ASM;
        else if (strcmp(argv[i], "-c") == 0) mode = EMIT_OBJECT;
        else if (strcmp(argv[i], "--exe") == 0) mode = EMIT_EXE;
        else if (strcmp(argv[i], "-O0") == 0) level = 0;
        else if (strcmp(argv[i], "-O1") == 0) level = 1;
        else if (strcmp(argv[i], "-O2") == 0) level = 2;
        else if (strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
            /* 已消费的参数槽保存监听器路径，保持命令行顺序。 */
            argv[1 + meta_count++] = argv[i + 1];
            ++i;
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (input == NULL && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0)) input = argv[i];
        else { usage(stderr, argv[0]); return 2; }
    }
    if (!output && mode == EMIT_OBJECT) { output = "program.obj"; }
    if (!output && mode == EMIT_EXE) { output = "program.exe"; }
    if (input == NULL || (output != NULL && strcmp(input, output) == 0)) {
        usage(stderr, argv[0]); return 2;
    }
    AtriMetaPlugin **plugins = meta_count ? calloc(meta_count, sizeof(*plugins)) : NULL;
    AtriListener *listeners = meta_count ? calloc(meta_count, sizeof(*listeners)) : NULL;
    if (meta_count && (!plugins || !listeners)) { free(plugins); free(listeners); return 2; }
    size_t loaded = 0;
    for (; loaded < meta_count; ++loaded) {
        plugins[loaded] = atri_meta_open(argv[1 + loaded]);
        if (!plugins[loaded]) { break; }
        listeners[loaded] = (AtriListener){argv[1 + loaded], atri_meta_callback(plugins[loaded])};
    }
    int status = loaded == meta_count ? compile(mode, level, input, output, listeners, meta_count) : 1;
    atri_syntax_destroy();
    while (loaded) { atri_meta_close(plugins[--loaded]); }
    free(listeners);
    free(plugins);
    return status;
}
