#include "native_expect.h"
#include "control_flow_harness_bindings.h"

int main(void) {
    EXPECT_EQ(-1, atri_test_choose(-2));
    EXPECT_EQ(0, atri_test_choose(0));
    EXPECT_EQ(1, atri_test_choose(4));

    EXPECT_EQ(25, atri_test_while_sum(20));
    EXPECT_EQ(0, atri_test_while_sum(0));
    EXPECT_EQ(8, atri_test_for_sum(5));
    EXPECT_EQ(0, atri_test_for_sum(0));
    EXPECT_EQ(1, atri_test_do_count(0));
    EXPECT_EQ(5, atri_test_do_count(5));
    EXPECT_EQ(4, atri_test_nested_break(4));
    EXPECT_EQ(4, atri_test_endless_counter(4));
    EXPECT_EQ(4, atri_test_loop_counter(4));
    EXPECT_EQ(120, atri_test_factorial(5));

    int32_t values[] = {1, 2, 3, 4};
    EXPECT_EQ(10, atri_test_sum_pointer(values, values + 4));
    EXPECT_EQ(0, atri_test_sum_pointer(values, values));
    EXPECT_TRUE(atri_test_is_null(NULL));
    EXPECT_FALSE(atri_test_is_null(values));
    EXPECT_FALSE(atri_test_null_guard(NULL));
    EXPECT_TRUE(atri_test_null_guard(values));

    EXPECT_TRUE(atri_test_bools(true, false, true, false, true, true));
    EXPECT_FALSE(atri_test_bools(true, false, true, false, true, false));
    EXPECT_FALSE(atri_test_bools(true, false, true, true, true, true));
    EXPECT_TRUE(atri_test_call_bools());
    EXPECT_EQ(2, atri_test_short_circuit());
    EXPECT_FALSE(atri_test_safe_divide(0));
    EXPECT_TRUE(atri_test_safe_divide(2));
    EXPECT_FALSE(atri_test_safe_divide(10));
    EXPECT_TRUE(atri_test_bool_loop(4));

    EXPECT_EQ(5, atri_test_scoped());
    EXPECT_EQ(INT64_C(15000000000), atri_test_wide_loop(3));
    EXPECT_EQ(163, atri_test_computed());
    EXPECT_TRUE(atri_test_compare_wide(INT64_MIN, INT64_MAX));
    EXPECT_FALSE(atri_test_compare_wide(INT64_MAX, INT64_MIN));
    EXPECT_EQ(5, atri_test_bind_before_branch(-1));
    EXPECT_EQ(7, atri_test_bind_before_branch(0));
    return 0;
}
