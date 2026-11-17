#include "basic_types_harness_bindings.h"
#include "native_expect.h"
#include <stdarg.h>
#include <string.h>

/* 每个宽度从 C 传动态参数，防止仅验证到编译期折叠；邻接元素检查窄写入宽度。 */
#define CHECK_SIGNED(name, ctype, minimum, maximum) do { \
    EXPECT_EQ(minimum, atri_test_add_##name(maximum, 1)); \
    EXPECT_EQ(-2, atri_test_divide_##name(-7, 3)); \
    EXPECT_EQ(-1, atri_test_remainder_##name(-7, 3)); \
    EXPECT_EQ(2, atri_test_bits_##name(3, 5, 1)); \
    EXPECT_EQ(-1, atri_test_bits_##name(-2, -1, sizeof(ctype)*8-1)); \
    ctype values[3] = {11, 22, 33}; \
    EXPECT_EQ(minimum, atri_test_write_##name(values+1, minimum)); \
    EXPECT_EQ(11, values[0]); EXPECT_EQ(minimum, values[1]); EXPECT_EQ(33, values[2]); \
    EXPECT_PTR_EQ(values+1, atri_test_step_##name(values, 2)); \
    EXPECT_EQ(2, atri_test_distance_##name(values+2, values)); \
    EXPECT_EQ(-2, atri_test_distance_##name(values, values+2)); \
    EXPECT_TRUE(atri_test_widen_##name(-1) == UINT64_MAX); \
    EXPECT_EQ(-1, atri_test_narrow_##name(UINT64_MAX)); \
} while (0)

#define CHECK_UNSIGNED(name, ctype, maximum) do { \
    EXPECT_EQ(0, atri_test_add_##name(maximum, 1)); \
    EXPECT_TRUE(atri_test_divide_##name(maximum, 3) == (ctype)((maximum)/3)); \
    EXPECT_EQ((maximum)%3, atri_test_remainder_##name(maximum, 3)); \
    EXPECT_EQ(2, atri_test_bits_##name(3, 5, 1)); \
    EXPECT_TRUE(atri_test_bits_##name(0, maximum, sizeof(ctype)*8-1) == (maximum)); \
    ctype values[3] = {11, 22, 33}; \
    EXPECT_TRUE(atri_test_write_##name(values+1, maximum) == (maximum)); \
    EXPECT_EQ(11, values[0]); EXPECT_TRUE(values[1] == (maximum)); EXPECT_EQ(33, values[2]); \
    EXPECT_PTR_EQ(values+1, atri_test_step_##name(values, 2)); \
    EXPECT_EQ(2, atri_test_distance_##name(values+2, values)); \
    EXPECT_EQ(-2, atri_test_distance_##name(values, values+2)); \
    EXPECT_TRUE(atri_test_widen_##name(maximum) == (uint64_t)(maximum)); \
    EXPECT_TRUE(atri_test_narrow_##name(UINT64_MAX) == (maximum)); \
} while (0)

atri_test_Mixed c_mixed(atri_test_Mixed value) {
    EXPECT_EQ('A', value.character); EXPECT_EQ(-128, value.small);
    EXPECT_EQ(-32768, value.signed_short); EXPECT_EQ(65535, value.unsigned_short);
    EXPECT_TRUE(value.flag); EXPECT_EQ(UINT32_MAX, value.unsigned_int);
    EXPECT_EQ(-1, value.signed_long); EXPECT_TRUE(value.unsigned_long == UINT64_MAX);
    value.character = 'B'; value.small = 7; value.signed_short = -9;
    value.unsigned_short = 12345; value.flag = false; value.unsigned_int = 3000000000U;
    return value;
}
int16_t c_narrow(int8_t a, uint16_t b, char c, uint32_t d, int16_t e, uint64_t f) {
    EXPECT_EQ(-128, a); EXPECT_EQ(65535, b); EXPECT_EQ(-1, c);
    EXPECT_EQ(UINT32_MAX, d); EXPECT_EQ(-32768, e); EXPECT_TRUE(f == UINT64_MAX);
    return -32767;
}
int32_t c_numbers(int32_t tag, ...) {
    EXPECT_EQ(8, tag);
    va_list arguments;
    va_start(arguments, tag);
    EXPECT_EQ(-128, va_arg(arguments, int));
    EXPECT_EQ(-32768, va_arg(arguments, int));
    EXPECT_EQ(65535, va_arg(arguments, int));
    EXPECT_EQ(UINT32_MAX, va_arg(arguments, uint32_t));
    EXPECT_EQ(255, va_arg(arguments, int));
    EXPECT_EQ(1, va_arg(arguments, int));
    EXPECT_EQ(-7, va_arg(arguments, int64_t));
    EXPECT_TRUE(va_arg(arguments, uint64_t) == UINT64_MAX);
    va_end(arguments);
    return 0;
}
int32_t c_literal(char *text) {
    const unsigned char expected[] = {'A', 'B', 'C', 0, 't', 'a', 'i', 'l', 255, 0};
    EXPECT_TRUE(memcmp(expected, text, sizeof(expected)) == 0);
    return 0;
}
int main(void) {
    _Static_assert(sizeof(char) == 1 && (char)-1 < 0, "Windows x64 default C char");
    _Static_assert(sizeof(atri_test_Mixed) == 32 && offsetof(atri_test_Mixed, unsigned_int) == 8 &&
        offsetof(atri_test_Mixed, signed_long) == 16, "target scalar layout");
    CHECK_SIGNED(i8, int8_t, INT8_MIN, INT8_MAX);
    CHECK_SIGNED(i16, int16_t, INT16_MIN, INT16_MAX);
    CHECK_SIGNED(i32, int32_t, INT32_MIN, INT32_MAX);
    CHECK_SIGNED(i64, int64_t, INT64_MIN, INT64_MAX);
    CHECK_UNSIGNED(u8, uint8_t, UINT8_MAX);
    CHECK_UNSIGNED(u16, uint16_t, UINT16_MAX);
    CHECK_UNSIGNED(u32, uint32_t, UINT32_MAX);
    CHECK_UNSIGNED(u64, uint64_t, UINT64_MAX);
    atri_test_Mixed value = atri_test_mixed_roundtrip(atri_test_mixed_defaults());
    EXPECT_EQ('B', value.character); EXPECT_EQ(7, value.small); EXPECT_EQ(-9, value.signed_short);
    EXPECT_EQ(12345, value.unsigned_short); EXPECT_FALSE(value.flag); EXPECT_EQ(3000000000U, value.unsigned_int);
    EXPECT_EQ(-32767, atri_test_narrow_stack(-128, 65535, (char)255, UINT32_MAX, -32768, UINT64_MAX));
    EXPECT_EQ(0, atri_test_call_variadic());
    uint8_t characters[3] = {'A', 'B', 'C'};
    EXPECT_EQ(255, atri_test_write_char(characters+1, 255));
    EXPECT_EQ('A', characters[0]); EXPECT_EQ(255, characters[1]); EXPECT_EQ('C', characters[2]);
    EXPECT_EQ(255, atri_test_char_number(255));
    EXPECT_FALSE(atri_test_compare_char(255, 0));
    bool flags[3] = {true, false, true};
    EXPECT_TRUE(atri_test_write_bool(flags+1, true));
    EXPECT_TRUE(flags[0] && flags[1] && flags[2]);
    EXPECT_EQ(0, atri_test_call_literal());
    EXPECT_EQ(0, atri_test_compound_order());
    EXPECT_EQ(0, atri_test_all_compounds());
    EXPECT_EQ(0, atri_test_arrays_and_globals());
    EXPECT_EQ(0, atri_test_default_and_address());
    EXPECT_EQ(0, atri_test_byte_literal_index());
    const uint8_t fixed_hex[] = {'A', 'B', 0, '1', '2', 0};
    EXPECT_TRUE(memcmp(fixed_hex, atri_test_fixed_hex_literal(), sizeof(fixed_hex)) == 0);
    return 0;
}
