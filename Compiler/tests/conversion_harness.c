#include "conversion_harness_bindings.h"
#include "native_expect.h"

uint64_t c_unsigned(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    EXPECT_TRUE(a == UINT64_MAX); EXPECT_TRUE(b == (UINT64_C(1) << 63));
    EXPECT_EQ(3, c); EXPECT_EQ(4, d); EXPECT_TRUE(e == UINT64_MAX);
    return e;
}
int main(void) {
    EXPECT_EQ(-1, atri_test_widen_signed(-1));
    EXPECT_EQ(INT32_MIN, atri_test_widen_signed(INT32_MIN));
    EXPECT_TRUE(atri_test_signed_to_unsigned(-1) == UINT64_MAX);
    EXPECT_EQ(255, atri_test_widen_byte(255));
    EXPECT_EQ(44, atri_test_narrow_byte(300));
    EXPECT_EQ(255, atri_test_narrow_byte(UINT64_MAX));
    EXPECT_EQ(-1, atri_test_narrow_signed(INT64_C(4294967295)));
    EXPECT_EQ(INT32_MIN, atri_test_narrow_signed(INT64_C(2147483648)));
    EXPECT_EQ(-1, atri_test_unsigned_to_signed(UINT64_MAX));
    EXPECT_EQ(-1, atri_test_unsigned_to_i32(UINT64_MAX));
    EXPECT_TRUE(atri_test_signed_bits(-1) == UINT64_MAX);
    EXPECT_TRUE(atri_test_add(UINT64_MAX, 2) == 1);
    EXPECT_TRUE(atri_test_sub(0, 1) == UINT64_MAX);
    EXPECT_TRUE(atri_test_mul(UINT64_C(1) << 63, 3) == (UINT64_C(1) << 63));
    EXPECT_TRUE(atri_test_negate(1) == UINT64_MAX);
    EXPECT_TRUE(atri_test_divide(UINT64_MAX, 2) == INT64_MAX);
    EXPECT_TRUE(atri_test_remainder(UINT64_MAX, 7) == 1);
    EXPECT_FALSE(atri_test_less(UINT64_MAX, 0));
    EXPECT_TRUE(atri_test_less(0, UINT64_MAX));
    EXPECT_TRUE(atri_test_less_equal(UINT64_MAX, UINT64_MAX));
    EXPECT_TRUE(atri_test_greater(UINT64_MAX, 0));
    EXPECT_FALSE(atri_test_greater_equal(0, UINT64_MAX));
    EXPECT_TRUE(atri_test_constant_bits() == UINT64_MAX);
    EXPECT_TRUE(atri_test_constant_divide() == INT64_MAX);
    EXPECT_EQ(255, atri_test_chained(UINT64_MAX));
    EXPECT_EQ(0, atri_test_use_layout());
    return 0;
}
