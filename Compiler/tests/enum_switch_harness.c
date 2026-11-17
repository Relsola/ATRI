#include "enum_switch_harness_bindings.h"
#include "native_expect.h"
#include <stdarg.h>

_Static_assert(sizeof(atri_test_Packet) == 16 && offsetof(atri_test_Packet, flags) == 4 &&
    offsetof(atri_test_Packet, wide) == 8, "enum storage is its integer type");
atri_test_Packet c_packet(atri_test_Packet value) { value.color += 1; value.flags |= 2; return value; }
uint64_t c_enum(int32_t tag, ...) {
    va_list args; va_start(args, tag);
    int32_t flags = va_arg(args, int32_t);
    uint64_t wide = va_arg(args, uint64_t);
    va_end(args); return (uint64_t)tag+(uint64_t)flags+wide;
}
int main(void) {
    EXPECT_EQ(10, atri_test_choose(-1)); EXPECT_EQ(10, atri_test_choose(0));
    EXPECT_EQ(17, atri_test_choose(1)); EXPECT_EQ(5, atri_test_choose(2)); EXPECT_EQ(-1, atri_test_choose(3));
    EXPECT_EQ(11, atri_test_evaluate_once()); EXPECT_EQ(46, atri_test_loop_control());
    EXPECT_EQ(111, atri_test_nested(1)); EXPECT_EQ(100, atri_test_nested(0));
    EXPECT_EQ(7, atri_test_empty(7));
    EXPECT_EQ(1, atri_test_default_middle(0)); EXPECT_EQ(3, atri_test_default_middle(1)); EXPECT_EQ(5, atri_test_default_middle(2));
    EXPECT_EQ(4, atri_test_index(0)); EXPECT_EQ(6, atri_test_index(2)); EXPECT_EQ(15, atri_test_arithmetic(10));
    EXPECT_EQ(2, atri_test_flags(1)); EXPECT_EQ(7, atri_test_flags(4));
    EXPECT_EQ(0, atri_test_flag_zero()); EXPECT_EQ(0, atri_test_flags_default());
    EXPECT_TRUE(atri_test_big(3) == UINT64_C(0x8000000000000003));
    EXPECT_EQ(-128, atri_test_signed8()); EXPECT_EQ(-32768, atri_test_signed16());
    EXPECT_TRUE(atri_test_signed64() == INT64_MIN);
    EXPECT_EQ(65535, atri_test_unsigned16()); EXPECT_EQ(UINT32_MAX, atri_test_unsigned32());
    EXPECT_TRUE(atri_test_unsigned64() == UINT64_MAX);
    EXPECT_EQ(1, atri_test_wide_case(UINT64_MAX)); EXPECT_EQ(2, atri_test_wide_case(0)); EXPECT_EQ(3, atri_test_wide_case(9));
    atri_test_Packet value = atri_test_read_global();
    EXPECT_EQ(2, value.color); EXPECT_EQ(5, value.flags); EXPECT_TRUE(value.wide == UINT64_C(0x8000000000000000));
    atri_test_Packet result = atri_test_cross(value);
    EXPECT_EQ(3, result.color); EXPECT_EQ(7, result.flags); EXPECT_TRUE(result.wide == value.wide);
    EXPECT_EQ(0, atri_test_variadic(0,UINT64_MAX));
    atri_test_global_address()->color = 1;
    EXPECT_EQ(1, atri_test_read_global().color);
    return 0;
}
