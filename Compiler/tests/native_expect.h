#ifndef ATRI_NATIVE_EXPECT_H
#define ATRI_NATIVE_EXPECT_H
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* 比较放在函数中，确保宏实参中的调用或 ++ 只执行一次；宏仅额外保留原表达式与位置。 */
static inline void native_expect_equal(int64_t expected, int64_t actual,
                                        const char *expression, const char *file, int line) {
    if (actual == expected) return;
    fprintf(stderr, "%s:%d: %s\n  expected: %" PRId64 "\n  actual:   %" PRId64 "\n",
            file, line, expression, expected, actual);
    exit(EXIT_FAILURE);
}
static inline void native_expect_pointer(const void *expected, const void *actual,
                                          const char *expression, const char *file, int line) {
    if (actual == expected) return;
    fprintf(stderr, "%s:%d: %s\n  expected: %p\n  actual:   %p\n",
            file, line, expression, (void *)expected, (void *)actual);
    exit(EXIT_FAILURE);
}
#define EXPECT_EQ(expected, actual) native_expect_equal(expected, actual, #actual, __FILE__, __LINE__)
#define EXPECT_PTR_EQ(expected, actual) native_expect_pointer(expected, actual, #actual, __FILE__, __LINE__)
#define EXPECT_TRUE(actual) EXPECT_EQ(true, !!(actual))
#define EXPECT_FALSE(actual) EXPECT_EQ(false, !!(actual))
#endif
