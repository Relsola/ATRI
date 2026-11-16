#include "test_support.h"

static void inline_add(TestMode mode) {
    Compilation test = test_compile_file("cases/optimization/inline_add.txt", mode);
    EXPECT_CALL(test.before, "main", "add");
    if (mode == TEST_O1) {
        EXPECT_NO_CALLS(test.after, "main");
        EXPECT_RETURN_I32(test.after, "main", 3);
    } else {
        EXPECT_CALL(test.after, "main", "add");
    }
    test_destroy(&test);
}

static void propagation_and_call_chain(void) {
    Compilation test = test_compile_file("cases/optimization/mutable_inline.txt", TEST_O1);
    EXPECT_CALL(test.before, "main", "twice");
    EXPECT_NO_CALLS(test.after, "main");
    EXPECT_RETURN_I32(test.after, "main", 6);
    test_destroy(&test);

    test = test_compile_file("cases/optimization/call_chain.txt", TEST_O1);
    EXPECT_CALL(test.before, "main", "a");
    EXPECT_NO_CALLS(test.after, "main");
    EXPECT_RETURN_I32(test.after, "main", 3);
    test_destroy(&test);
}

static void constant_call(TestMode mode) {
    Compilation test = test_compile_file("cases/optimization/constant_call.txt", mode);
    EXPECT_NO_CALLS(test.before, "main");
    EXPECT_RETURN_I32(test.before, "main", 3);
    EXPECT_NO_CALLS(test.after, "main");
    EXPECT_RETURN_I32(test.after, "main", 3);
    test_destroy(&test);
}

static void dead_code(TestMode mode) {
    Compilation test = test_compile_file("cases/optimization/dead_calculation.txt", mode);
    EXPECT_OPERATIONS(test.before, "main", ATRI_IR_BINARY, 1);
    EXPECT_OPERATIONS(test.after, "main", ATRI_IR_BINARY, 0);
    EXPECT_RETURN_I32(test.after, "main", 7);
    test_destroy(&test);

    test = test_compile_file("cases/optimization/dead_tail.txt", mode);
    EXPECT_RETURN_I32(test.before, "main", 1);
    EXPECT_OPERATIONS(test.before, "main", ATRI_IR_RETURN, 1);
    EXPECT_RETURN_I32(test.after, "main", 1);
    test_destroy(&test);
}

static void preserve_observable_behavior(TestMode mode) {
    Compilation test = test_compile_file("cases/optimization/preserve_store.txt", mode);
    EXPECT_CALL(test.before, "main", "write");
    EXPECT_CALL(test.after, "main", "write");
    EXPECT_OPERATIONS(test.after, "write", ATRI_IR_STORE, 1);
    EXPECT_OPERATIONS(test.after, "main", ATRI_IR_LOAD, 1);
    test_destroy(&test);

    test = test_compile_file("cases/optimization/recursive_calls.txt", mode);
    EXPECT_CALL(test.after, "a", "b");
    EXPECT_CALL(test.after, "b", "a");
    test_destroy(&test);
}

int main(void) {
    for (TestMode mode = TEST_O0; mode <= TEST_O1; ++mode) {
        inline_add(mode);
        constant_call(mode);
        dead_code(mode);
        preserve_observable_behavior(mode);
    }
    propagation_and_call_chain();
    EXPECT_OPT_ERROR(
        "fn main() -> i32 { return divide(0); }"
        "fn divide(x: i32) -> i32 { return 10 / x; }",
        "by zero");
    puts("optimization tests passed");
    return 0;
}
