#include "native_expect.h"
#include "pointer_arithmetic_harness_bindings.h"

int main(void) {
    int32_t values[8] = {10, 20, 30, 40, 50, 60, 70, 80};

    EXPECT_PTR_EQ(values + 3, atri_test_add32(values, 3));
    EXPECT_PTR_EQ(values + 1, atri_test_add32(values + 4, -3));
    EXPECT_PTR_EQ(values + 2, atri_test_reverse_add32(-2, values + 4));
    EXPECT_PTR_EQ(values + 1, atri_test_subtract32(values + 4, 3));
    EXPECT_PTR_EQ(values + 6, atri_test_subtract32(values + 4, -2));
    EXPECT_PTR_EQ(values + 8, atri_test_add64(values, 8));
    EXPECT_PTR_EQ(values + 1, atri_test_add64(values + 4, -3));
    EXPECT_PTR_EQ(values, atri_test_subtract64(values + 8, 8));
    EXPECT_PTR_EQ(values + 6, atri_test_subtract64(values + 4, -2));

    EXPECT_EQ(8, atri_test_distance(values + 8, values));
    EXPECT_EQ(-6, atri_test_distance(values + 1, values + 7));
    EXPECT_EQ(0, atri_test_distance(values, values));
    EXPECT_EQ(30, atri_test_read_offset(values + 4, -2));
    EXPECT_EQ(99, atri_test_write_offset(values, 3, 99));
    EXPECT_EQ(99, values[3]);

    EXPECT_PTR_EQ(values, atri_test_roundtrip(values, 8));
    EXPECT_EQ(-3, atri_test_count(values + 4, -3));
    EXPECT_PTR_EQ(values + 3, atri_test_inline_offset(values + 4, -1));
    EXPECT_PTR_EQ(values, atri_test_evaluation_order(values));
    EXPECT_EQ(5, values[0]);

    EXPECT_EQ(INT64_C(-7500000001), atri_test_wide_arithmetic(INT64_C(5000000000), 7));
    EXPECT_EQ(INT64_C(7499999994), atri_test_wide_arithmetic(INT64_C(-5000000000), 7));
    EXPECT_EQ(INT64_C(18000000004), atri_test_mixed_stack(
        INT64_C(5000000000), 1, values + 1, INT64_C(6000000000), INT64_C(7000000000), values + 5));
    EXPECT_EQ(INT64_C(10000000001), atri_test_constant64());
    EXPECT_EQ(INT64_MIN, atri_test_wrap64(INT64_MAX));
    return 0;
}
