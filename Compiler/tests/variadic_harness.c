#include "variadic_harness_bindings.h"
#include "native_expect.h"
#include <stdarg.h>
#include <stdio.h>

int32_t c_args(int32_t tag, ...) {
    va_list args;
    va_start(args, tag);
    EXPECT_EQ(17, tag);
    EXPECT_EQ(-123, va_arg(args, int));
    EXPECT_TRUE(va_arg(args, uint64_t) == UINT64_MAX);
    int32_t *pointer = va_arg(args, void *);
    EXPECT_EQ(6, *pointer);
    EXPECT_EQ(255, va_arg(args, int));
    EXPECT_EQ(1, va_arg(args, int));
    atri_test_Small small = va_arg(args, atri_test_Small);
    atri_test_Big big = va_arg(args, atri_test_Big);
    EXPECT_EQ(-7, small.a); EXPECT_EQ(8, small.b);
    EXPECT_EQ(-9, big.a); EXPECT_EQ(10, big.b); EXPECT_EQ(11, big.c);
    va_end(args);
    return 0;
}
atri_test_Big c_big(int32_t tag, ...) {
    va_list args;
    va_start(args, tag);
    EXPECT_EQ(18, tag);
    EXPECT_EQ(255, va_arg(args, int)); EXPECT_EQ(1, va_arg(args, int));
    EXPECT_EQ(-456, va_arg(args, int)); EXPECT_TRUE(va_arg(args, uint64_t) == UINT64_MAX);
    atri_test_Big result = va_arg(args, atri_test_Big);
    va_end(args);
    return result;
}
void c_void(int32_t tag, ...) {
    va_list args;
    va_start(args, tag);
    EXPECT_EQ(19, tag);
    int32_t *pointer = va_arg(args, void *);
    EXPECT_EQ(255, va_arg(args, int)); EXPECT_EQ(1, va_arg(args, int));
    *pointer = 99;
    va_end(args);
}
int32_t c_only(...) { return 23; }
int32_t c_order(int32_t tag, ...) {
    va_list args;
    va_start(args, tag);
    EXPECT_EQ(20, tag);
    for (int i = 1; i <= 4; ++i) { EXPECT_EQ(i, va_arg(args, int)); }
    va_end(args);
    return 0;
}

int main(int argc, char **argv) {
    EXPECT_EQ(3, argc);
    EXPECT_EQ(0, atri_test_check_variadic());
    int32_t number = 1;
    EXPECT_PTR_EQ(&number, atri_test_opaque_roundtrip((atri_test_Handle *)&number));
    atri_test_void_return(&number, true); EXPECT_EQ(7, number);
    atri_test_void_return(&number, false); EXPECT_EQ(8, number);
    EXPECT_EQ(9, atri_test_constant_void());
    FILE *file = fopen(argv[1], "wb");
    EXPECT_TRUE(file != NULL);
    for (size_t i = 0; i < 257; ++i) { EXPECT_TRUE(fputc((unsigned char)i, file) != EOF); }
    EXPECT_EQ(0, fclose(file));
    EXPECT_EQ(0, atri_test_check_file(argv[1], argv[2]));
    EXPECT_EQ(0, atri_test_check_write(argv[1]));
    file = fopen(argv[1], "rb"); EXPECT_TRUE(file != NULL);
    const unsigned char expected[] = {0, 128, 255, 10};
    for (size_t i = 0; i < sizeof(expected); ++i) { EXPECT_EQ(expected[i], fgetc(file)); }
    EXPECT_EQ(EOF, fgetc(file)); EXPECT_EQ(0, fclose(file));
    EXPECT_EQ(0, remove(argv[1]));
    return 0;
}
