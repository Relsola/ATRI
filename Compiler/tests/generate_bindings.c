#include "test_support.h"

/* 仅供测试：使用与后端相同的函数顺序映射符号，具名用例无需手写编号；不定义正式 FFI 命名规则。 */
static const char *c_type(AtriIRType type) {
    static char name[80];
    if (atri_ir_is_struct(type) || type >= ATRI_IR_PTR_STRUCT) {
        snprintf(name, sizeof(name), "struct atri_struct_%zu%s", (size_t)(type >= ATRI_IR_PTR_STRUCT ? type-ATRI_IR_PTR_STRUCT : type-ATRI_IR_STRUCT), type >= ATRI_IR_PTR_STRUCT ? " *" : "");
        return name;
    }
    switch (type) {
        case ATRI_IR_I32: return "int32_t";
        case ATRI_IR_I64: return "int64_t";
        case ATRI_IR_BOOL: return "bool";
        case ATRI_IR_PTR_I32: return "int32_t *";
        case ATRI_IR_PTR_CHAR: return "char *";
        default: fputs("unsupported test binding type\n", stderr); exit(EXIT_FAILURE);
    }
}

static void emit_struct(FILE *header, const TestIR *ir, size_t index, bool *emitted) {
    if (emitted[index]) { return; }
    const AtriIRStruct *t = &ir->program.structs[index];
    if (!t->size) { fputs("zero-size structs have no C test binding\n", stderr); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < t->field_count; ++i) {
        AtriIRType type = ir->program.fields[t->first_field+i].type;
        if (atri_ir_is_struct(type)) { emit_struct(header, ir, type-ATRI_IR_STRUCT, emitted); }
    }
    fprintf(header, "struct atri_struct_%zu {\n", index);
    for (size_t i = 0; i < t->field_count; ++i) {
        const AtriIRField *field = &ir->program.fields[t->first_field+i];
        fprintf(header, "    %s %.*s;\n", c_type(field->type), (int)(field->name.end-field->name.start), ir->source.text+field->name.start);
    }
    fputs("};\n", header); emitted[index] = true;
}
int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: test-bindings source output-header\n", stderr);
        return 2;
    }
    Compilation test = test_compile_file(argv[1], TEST_NO_OPT);
    FILE *header = fopen(argv[2], "wb");
    if (!header) { fprintf(stderr, "cannot write test bindings: %s\n", argv[2]); test_destroy(&test); return 2; }
    fputs("/* Generated from the test source; do not edit. */\n#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n", header);
    bool *emitted = calloc(test.before.program.struct_count ? test.before.program.struct_count : 1, sizeof(*emitted));
    if (!emitted) { fclose(header); test_destroy(&test); return 2; }
    for (size_t i = 0; i < test.before.program.struct_count; ++i) {
        AtriSpan name = test.before.program.structs[i].name;
        fprintf(header, "typedef struct atri_struct_%zu atri_test_%.*s;\n", i, (int)(name.end-name.start), test.before.source.text+name.start);
    }
    for (size_t i = 0; i < test.before.program.struct_count; ++i) { emit_struct(header, &test.before, i, emitted); }
    free(emitted);
    for (size_t i = 0; i < test.before.program.function_count; ++i) {
        const AtriIRFunction *function = &test.before.program.functions[i];
        if (function->external_c) {
            continue;
        }
        int length = (int)(function->name.end-function->name.start);
        const char *name = test.before.source.text+function->name.start;
        fprintf(header, "#define atri_test_%.*s atri_fn_%zu\n", length, name, i);
        fprintf(header, "extern %s atri_test_%.*s(", c_type(function->return_type), length, name);
        if (!function->parameter_count) fputs("void", header);
        for (size_t a = 0; a < function->parameter_count; ++a)
            fprintf(header, "%s%s", a ? ", " : "", c_type(function->parameter_types[a]));
        fputs(");\n", header);
    }
    bool ok = !ferror(header);
    if (fclose(header) != 0) ok = false;
    test_destroy(&test);
    return ok ? 0 : 2;
}
