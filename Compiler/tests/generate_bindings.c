#include "test_support.h"

/* 仅供测试：使用与后端相同的函数顺序映射符号，具名用例无需手写编号；不定义正式 FFI 命名规则。 */
static const char *c_builtin_type(AtriIRType type) {
    switch (type) {
        case ATRI_IR_VOID: return "void";
        case ATRI_IR_I8: return "int8_t";
        case ATRI_IR_I16: return "int16_t";
        case ATRI_IR_U16: return "uint16_t";
        case ATRI_IR_U32: return "uint32_t";
        case ATRI_IR_U64: return "uint64_t";
        case ATRI_IR_U8: return "uint8_t";
        case ATRI_IR_I32: return "int32_t";
        case ATRI_IR_I64: return "int64_t";
        case ATRI_IR_BOOL: return "bool";
        default: fputs("unsupported test binding type\n", stderr); exit(EXIT_FAILURE);
    }
}

static const char *c_type(const AtriIRProgram *p, AtriIRType type) {
    static char name[80 + ATRI_IR_MAX_POINTER_DEPTH];
    size_t depth = 0;
    while (atri_ir_is_pointer(type)) { ++depth; type = atri_ir_pointee(type); }
    size_t length;
    if (atri_ir_is_struct(type)) {
        size_t index = type-ATRI_IR_STRUCT;
        length = (size_t)snprintf(name, sizeof(name), "%s atri_struct_%zu",
            p->structs[index].is_union ? "union" : "struct", index);
    } else {
        const char *base = c_builtin_type(type);
        length = strlen(base);
        memcpy(name, base, length);
    }
    if (depth) { name[length++] = ' '; }
    memset(name+length, '*', depth);
    name[length+depth] = '\0';
    return name;
}

static void emit_struct(FILE *header, const TestIR *ir, size_t index, bool *emitted) {
    if (emitted[index]) { return; }
    const AtriIRStruct *t = &ir->program.structs[index];
    if (t->opaque) { emitted[index] = true; return; }
    if (!t->size) { fputs("zero-size structs have no C test binding\n", stderr); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < t->field_count; ++i) {
        AtriIRType type = ir->program.fields[t->first_field+i].type;
        while (atri_ir_is_array(type)) { type = ir->program.arrays[type-ATRI_IR_ARRAY].element; }
        if (atri_ir_is_struct(type)) { emit_struct(header, ir, type-ATRI_IR_STRUCT, emitted); }
    }
    fprintf(header, "%s atri_struct_%zu {\n", t->is_union ? "union" : "struct", index);
    for (size_t i = 0; i < t->field_count; ++i) {
        const AtriIRField *field = &ir->program.fields[t->first_field+i];
        AtriIRType element = field->type;
        while (atri_ir_is_array(element)) { element = ir->program.arrays[element-ATRI_IR_ARRAY].element; }
        fprintf(header, "    %s ", c_type(&ir->program, element));
        if (field->builtin_name) { fputs(field->builtin_name, header); }
        else { fprintf(header, "%.*s", (int)(field->name.end-field->name.start), ir->source.text+field->name.start); }
        for (AtriIRType type = field->type; atri_ir_is_array(type); type = ir->program.arrays[type-ATRI_IR_ARRAY].element) {
            fprintf(header, "[%zu]", ir->program.arrays[type-ATRI_IR_ARRAY].length);
        }
        fputs(";\n", header);
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
        fprintf(header, "typedef %s atri_struct_%zu atri_test_%.*s;\n", test.before.program.structs[i].is_union ? "union" : "struct",
            i, (int)(name.end-name.start), test.before.source.text+name.start);
    }
    for (size_t i = 0; i < test.before.program.struct_count; ++i) { emit_struct(header, &test.before, i, emitted); }
    free(emitted);
    fputs("static void *atri_test_context[2];\n", header);
    for (size_t i = 0; i < test.before.program.function_count; ++i) {
        const AtriIRFunction *function = &test.before.program.functions[i];
        if (function->external_c) {
            continue;
        }
        int length = (int)(function->name.end-function->name.start);
        const char *name = test.before.source.text+function->name.start;
        fprintf(header, "extern %s atri_fn_%zu(", c_type(&test.before.program, function->return_type), i);
        if (!function->parameter_count) fputs("void", header);
        for (size_t a = 0; a < function->parameter_count; ++a)
            fprintf(header, "%s%s", a ? ", " : "", c_type(&test.before.program, function->parameter_types[a]));
        fputs(");\n", header);
        fprintf(header, "#define atri_test_%.*s(...) atri_fn_%zu(__VA_ARGS__", length, name, i);
        if (function->has_context) { fputs(" __VA_OPT__(,) atri_test_context", header); }
        fputs(")\n", header);
    }
    bool ok = !ferror(header);
    if (fclose(header) != 0) ok = false;
    test_destroy(&test);
    return ok ? 0 : 2;
}
