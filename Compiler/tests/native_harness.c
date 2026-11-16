#include "native_expect.h"
#include "native_harness_bindings.h"

int main(void) {
    EXPECT_EQ(21, atri_test_sum6(1, 2, 3, 4, 5, 6));
    EXPECT_EQ(-32, atri_test_divrem(-17, 5));
    EXPECT_EQ(-28, atri_test_divrem(17, -5));
    EXPECT_EQ(INT32_MIN, atri_test_wrap(INT32_MAX, 1));
    EXPECT_EQ(INT32_MAX, atri_test_wrap(INT32_MIN, -1));
    EXPECT_EQ(20, atri_test_bindings(4));
    EXPECT_EQ(-4, atri_test_bindings(-4));
    EXPECT_EQ(-29, atri_test_nested(-17, 5));
    EXPECT_EQ(-1, atri_test_nested(17, -5));
    EXPECT_EQ(INT32_MIN, atri_test_negate(INT32_MIN));
    EXPECT_EQ(7, atri_test_negate(-7));
    return 0;
}
