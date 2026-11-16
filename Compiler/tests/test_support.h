#ifndef ATRI_TEST_SUPPORT_H
#define ATRI_TEST_SUPPORT_H
#include "ir.h"
#include <stdlib.h>
#include <string.h>

typedef enum { TEST_NO_OPT, TEST_O0, TEST_O1 } TestMode;
typedef struct {
    AtriSource source;
    AtriIRProgram program;
    const char *stage;
} TestIR;
typedef struct {
    TestIR before, after;
    char *owned_source; /* 仅文件用例拥有源码；内联用例借用调用者字符串，寿命须覆盖两份 IR。 */
} Compilation;

Compilation test_compile(const char *name, const char *code, TestMode mode);
Compilation test_compile_file(const char *path, TestMode mode);
void test_destroy(Compilation *test);
AtriIRFunction *test_function(const TestIR *ir, const char *name);
AtriId test_instruction(const TestIR *ir, const char *function, AtriIRKind kind, size_t occurrence);

void test_check(bool condition, const char *expression, const char *file, int line);
void test_expect_error(const char *code, const char *message, TestMode mode, bool syntax,
                       const char *file, int line);
void test_expect_return(const TestIR *ir, const char *function, AtriIRType type, int64_t expected,
                        const char *file, int line);
void test_expect_calls(const TestIR *ir, const char *function, const char *callee, size_t count,
                       const char *file, int line);
void test_expect_operations(const TestIR *ir, const char *function, AtriIRKind kind, size_t count,
                            const char *file, int line);
void test_expect_valid(const TestIR *ir, bool expected, const char *file, int line);

#define CHECK(c) test_check((c), #c, __FILE__, __LINE__)
#define EXPECT_PARSE_ERROR(code, message) test_expect_error(code, message, TEST_NO_OPT, true, __FILE__, __LINE__)
#define EXPECT_COMPILE_ERROR(code, message) test_expect_error(code, message, TEST_NO_OPT, false, __FILE__, __LINE__)
#define EXPECT_OPT_ERROR(code, message) test_expect_error(code, message, TEST_O1, false, __FILE__, __LINE__)
#define EXPECT_RETURN_I32(ir, function, value) test_expect_return(&(ir), function, ATRI_IR_I32, value, __FILE__, __LINE__)
#define EXPECT_RETURN_I64(ir, function, value) test_expect_return(&(ir), function, ATRI_IR_I64, value, __FILE__, __LINE__)
#define EXPECT_RETURN_BOOL(ir, function, value) test_expect_return(&(ir), function, ATRI_IR_BOOL, value, __FILE__, __LINE__)
#define EXPECT_CALL(ir, function, callee) test_expect_calls(&(ir), function, callee, 1, __FILE__, __LINE__)
#define EXPECT_NO_CALL(ir, function, callee) test_expect_calls(&(ir), function, callee, 0, __FILE__, __LINE__)
#define EXPECT_NO_CALLS(ir, function) test_expect_calls(&(ir), function, NULL, 0, __FILE__, __LINE__)
#define EXPECT_OPERATIONS(ir, function, kind, count) test_expect_operations(&(ir), function, kind, count, __FILE__, __LINE__)
#define EXPECT_VALID_IR(ir) test_expect_valid(&(ir), true, __FILE__, __LINE__)
#define EXPECT_INVALID_IR(ir) test_expect_valid(&(ir), false, __FILE__, __LINE__)
#endif
