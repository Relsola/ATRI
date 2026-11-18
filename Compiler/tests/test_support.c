#include "test_support.h"
#include <inttypes.h>

static void diagnostic_failure(AtriSource source, AtriDiagnostic diagnostic) {
    atri_print_diagnostic(stderr, source, diagnostic);
    exit(EXIT_FAILURE);
}
void test_check(bool condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "%s:%d: expected %s\n", file, line, expression);
        exit(EXIT_FAILURE);
    }
}
static bool named(AtriSource source, AtriSpan span, const char *name) {
    return span.end-span.start == strlen(name) &&
        memcmp(source.text+span.start, name, strlen(name)) == 0;
}
Compilation test_compile(const char *name, const char *code, TestMode mode) {
    Compilation test = {0};
    AtriSource source = {name, code, strlen(code)};
    AtriProgram ast;
    AtriDiagnostic diagnostic;
    if (!atri_parse(source, &ast, &diagnostic)) diagnostic_failure(source, diagnostic);
    test.before.source = test.after.source = source;
    test.before.stage = "before optimization";
    test.after.stage = mode == TEST_O1 ? "O1" : mode == TEST_O0 ? "O0" : "after lowering";
    /* 同一 AST 独立降低两份 IR，只优化 after，避免就地优化污染 before；两份都已完成 := 强制求值。 */
    if (!atri_lower(source, &ast, &test.before.program, &diagnostic) ||
        !atri_lower(source, &ast, &test.after.program, &diagnostic)) diagnostic_failure(source, diagnostic);
    atri_program_destroy(&ast);
    if (mode != TEST_NO_OPT && !atri_optimize(&test.after.program, mode == TEST_O1, &diagnostic))
        diagnostic_failure(source, diagnostic);
    return test;
}
Compilation test_compile_file(const char *path, TestMode mode) {
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "cannot open test source: %s\n", path); exit(EXIT_FAILURE); }
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *code = malloc((size_t)length+1);
    CHECK(code != NULL);
    CHECK(fread(code, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    code[length] = '\0';
    Compilation test = test_compile(path, code, mode);
    test.owned_source = code;
    return test;
}
void test_destroy(Compilation *test) {
    atri_ir_destroy(&test->before.program);
    atri_ir_destroy(&test->after.program);
    free(test->owned_source);
    *test = (Compilation){0};
}
AtriIRFunction *test_function(const TestIR *ir, const char *name) {
    for (size_t i = 0; i < ir->program.function_count; ++i)
        if (named(ir->source, ir->program.functions[i].name, name)) return &ir->program.functions[i];
    fprintf(stderr, "%s / %s: function %s not found\n", ir->source.name, ir->stage, name);
    exit(EXIT_FAILURE);
}
AtriId test_instruction(const TestIR *ir, const char *function, AtriIRKind kind, size_t occurrence) {
    AtriIRFunction *body = test_function(ir, function);
    for (size_t i = 0; i < body->instruction_count; ++i)
        if (body->instructions[i].kind == kind && occurrence-- == 0) return i;
    fprintf(stderr, "%s / %s: instruction kind %d not found\n", ir->source.name, function, (int)kind);
    exit(EXIT_FAILURE);
}
static void ir_failure(const TestIR *ir, const char *function, const char *file, int line) {
    fprintf(stderr, "%s:%d: %s / %s / %s\nActual IR:\n", file, line, ir->source.name, function, ir->stage);
    atri_dump_ir(stderr, ir->source, &ir->program);
    exit(EXIT_FAILURE);
}
void test_expect_return(const TestIR *ir, const char *function, AtriIRType type, int64_t expected,
                        const char *file, int line) {
    /* 检查每个 return 引用的实际值，不能只找“某条等于期望值的 const”；这里不执行被测程序。 */
    AtriIRFunction *body = test_function(ir, function);
    bool returned = false;
    uint64_t bits = type == ATRI_IR_I32 ? (uint32_t)expected : (uint64_t)expected;
    for (size_t i = 0; i < body->instruction_count; ++i) {
        AtriIRInst ret = body->instructions[i];
        if (ret.kind != ATRI_IR_RETURN) continue;
        returned = true;
        if (ret.left >= i || body->instructions[ret.left].kind != ATRI_IR_CONST ||
            ret.type != type || body->instructions[ret.left].type != type ||
            body->instructions[ret.left].immediate != bits) {
            char type_name[ATRI_IR_TYPE_NAME_SIZE];
            fprintf(stderr, "Expected: every return is constant %s %" PRId64 "\n", atri_ir_type_name(type, type_name), expected);
            ir_failure(ir, function, file, line);
        }
    }
    if (!returned) {
        fputs("Expected: a return instruction\n", stderr);
        ir_failure(ir, function, file, line);
    }
}
void test_expect_calls(const TestIR *ir, const char *function, const char *callee, size_t expected,
                       const char *file, int line) {
    /* 先确认被调函数名存在，避免拼错名字时“零次调用”断言意外通过。 */
    if (callee != NULL) (void)test_function(ir, callee);
    AtriIRFunction *body = test_function(ir, function);
    size_t actual = 0;
    for (size_t i = 0; i < body->instruction_count; ++i) {
        AtriIRInst inst = body->instructions[i];
        if (inst.kind == ATRI_IR_CALL && (callee == NULL ||
            named(ir->source, ir->program.functions[inst.callee].name, callee))) ++actual;
    }
    if (actual != expected) {
        fprintf(stderr, "Expected: %zu calls to %s; actual: %zu\n", expected, callee ? callee : "any function", actual);
        ir_failure(ir, function, file, line);
    }
}
void test_expect_operations(const TestIR *ir, const char *function, AtriIRKind kind, size_t expected,
                            const char *file, int line) {
    AtriIRFunction *body = test_function(ir, function);
    size_t actual = 0;
    for (size_t i = 0; i < body->instruction_count; ++i) actual += body->instructions[i].kind == kind;
    if (actual != expected) {
        fprintf(stderr, "Expected: %zu operations of kind %d; actual: %zu\n", expected, (int)kind, actual);
        ir_failure(ir, function, file, line);
    }
}
void test_expect_valid(const TestIR *ir, bool expected, const char *file, int line) {
    AtriDiagnostic diagnostic;
    bool actual = atri_ir_verify(&ir->program, &diagnostic);
    if (actual != expected) {
        fprintf(stderr, "%s:%d: %s: expected IR to be %s\n", file, line, ir->source.name, expected ? "valid" : "invalid");
        if (!actual) atri_print_diagnostic(stderr, ir->source, diagnostic);
        exit(EXIT_FAILURE);
    }
}
void test_expect_error(const char *code, const char *message, TestMode mode, bool syntax,
                       const char *file, int line) {
    AtriSource source = {file, code, strlen(code)};
    AtriProgram ast;
    AtriIRProgram ir = {0};
    AtriDiagnostic diagnostic;
    bool parsed = atri_parse(source, &ast, &diagnostic);
    bool accepted = parsed;
    if (syntax) {
        if (parsed) {
            fprintf(stderr, "%s:%d: expected a syntax error containing '%s'\nSource: %s\n", file, line, message, code);
            exit(EXIT_FAILURE);
        }
    } else {
        /* 语义/优化错误用例必须先通过语法，不能把漏写分号等解析失败当成预期类型错误。 */
        if (!parsed) diagnostic_failure(source, diagnostic);
        accepted = atri_lower(source, &ast, &ir, &diagnostic);
        if (accepted && mode != TEST_NO_OPT) accepted = atri_optimize(&ir, mode == TEST_O1, &diagnostic);
    }
    if (accepted || !diagnostic.message || !strstr(diagnostic.message, message)) {
        fprintf(stderr, "%s:%d: expected error containing '%s'\nSource: %s\n", file, line, message, code);
        if (!accepted) atri_print_diagnostic(stderr, source, diagnostic);
        exit(EXIT_FAILURE);
    }
    CHECK(diagnostic.span.start <= diagnostic.span.end && diagnostic.span.end <= source.length);
    atri_ir_destroy(&ir);
    atri_program_destroy(&ast);
}
