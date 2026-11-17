#include "array_harness_bindings.h"
#include "native_expect.h"
#include <stddef.h>

uint8_t c_byte(uint8_t *p, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    EXPECT_EQ(200, a); EXPECT_EQ(201, b); EXPECT_EQ(202, c); EXPECT_EQ(255, d);
    *p = 240;
    return d;
}
atri_test_Small c_small(atri_test_Small s) { ++s.bytes[0]; return s; }
atri_test_Big c_big(atri_test_Big s) { s.values[0] = 7; return s; }
int main(void) {
    _Static_assert(sizeof(atri_test_Small) == 4 && offsetof(atri_test_Small, tail) == 3, "byte array layout");
    _Static_assert(sizeof(atri_test_Big) == 24, "integer array layout");
    atri_test_Small small = atri_test_same_small((atri_test_Small){{200, 2, 3}, 255});
    EXPECT_EQ(200, small.bytes[0]); EXPECT_EQ(255, small.tail);
    atri_test_Big big = atri_test_same_big((atri_test_Big){{1, 2, 3}});
    EXPECT_EQ(1, big.values[0]); EXPECT_EQ(3, big.values[2]);
    EXPECT_EQ(0, atri_test_call_c());
    return 0;
}
