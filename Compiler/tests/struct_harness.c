#include "struct_harness_bindings.h"
#include "native_expect.h"
#include <stddef.h>

atri_test_Big c_change(atri_test_Big v, int32_t n, int64_t wide, atri_test_Pair pair, int32_t *pointer, bool flag) {
    v.a += n; v.b += wide; v.c += pair.a + pair.b + *pointer + flag;
    *pointer = 6; return v;
}
int64_t c_independent(atri_test_Big a, atri_test_Big b) { a.a = 99; return b.a + (a.a - 99); }
int main(void) {
    _Static_assert(sizeof(atri_test_B1) == 1 && sizeof(atri_test_B2) == 2 && sizeof(atri_test_B3) == 3, "bool struct layout");
    _Static_assert(sizeof(atri_test_Pair) == 8 && sizeof(atri_test_Big) == 24 && sizeof(atri_test_Nested) == 32, "aggregate sizes");
    _Static_assert(sizeof(atri_test_Mixed) == 16 && offsetof(atri_test_Mixed, pointer) == 8, "padding and pointer alignment");
    atri_test_B1 b1 = atri_test_same1((atri_test_B1){true}); EXPECT_TRUE(b1.a);
    atri_test_B2 b2 = atri_test_same2((atri_test_B2){true, false}); EXPECT_TRUE(b2.a); EXPECT_FALSE(b2.b);
    atri_test_B3 b3 = atri_test_same3((atri_test_B3){true, false, true}); EXPECT_TRUE(b3.a); EXPECT_FALSE(b3.b); EXPECT_TRUE(b3.c);
    EXPECT_EQ(-123, atri_test_same4((atri_test_I4){-123}).a);
    atri_test_Pair pair = atri_test_same8((atri_test_Pair){-7, 12345}); EXPECT_EQ(-7, pair.a); EXPECT_EQ(12345, pair.b);
    int32_t target = 9;
    atri_test_Mixed mixed = atri_test_same_mixed((atri_test_Mixed){-8, true, &target});
    EXPECT_EQ(-8, mixed.a); EXPECT_TRUE(mixed.flag); EXPECT_PTR_EQ(&target, mixed.pointer);
    atri_test_Big input = {10, 20, 30};
    atri_test_Big result = atri_test_adjust(input, 1, 2, 3, 4, 5);
    EXPECT_EQ(15, result.a); EXPECT_EQ(27, result.b); EXPECT_EQ(33, result.c); EXPECT_EQ(10, input.a);
    atri_test_Nested original = {{1, 2}, {3, 4, 5}};
    atri_test_Nested nested = atri_test_nested(original);
    EXPECT_EQ(42, nested.small.a); EXPECT_EQ(2, nested.small.b); EXPECT_EQ(77, nested.big.c);
    EXPECT_EQ(1, original.small.a); EXPECT_EQ(5, original.big.c);
    EXPECT_EQ(10, atri_test_independent(input));
    EXPECT_EQ(0, atri_test_call_c());
    atri_test_Big combined = atri_test_combine((atri_test_Big){1, 2, 3}, (atri_test_Big){5, 6, 7}, (atri_test_Pair){7, 8}, (atri_test_Mixed){8, true, &target}, 11);
    EXPECT_EQ(6, combined.a); EXPECT_EQ(8, combined.b); EXPECT_EQ(10, combined.c);
    return 0;
}
