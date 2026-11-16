#include "native_expect.h"
#include "pointer_harness_bindings.h"

int main(void) {
    int32_t x = 1, y = 2, z = 3;

    EXPECT_EQ(42, atri_test_write(&x, 42));
    EXPECT_EQ(42, x);
    EXPECT_EQ(7, atri_test_aliases(&x, &x));
    EXPECT_EQ(7, x);
    EXPECT_EQ(5, atri_test_aliases(&x, &y));
    EXPECT_EQ(5, x);
    EXPECT_EQ(4, y);

    EXPECT_PTR_EQ(&x, atri_test_identity(&x));
    EXPECT_EQ(10, atri_test_local_roundtrip(7));
    EXPECT_EQ(INT32_MIN + 2, atri_test_local_roundtrip(INT32_MAX));
    EXPECT_EQ(60, atri_test_mixed(10, &x, 20, &y, &z, 30));
    EXPECT_EQ(10, x);
    EXPECT_EQ(20, y);
    EXPECT_EQ(30, z);

    x = 1;
    EXPECT_EQ(10, atri_test_through_call(&x));
    EXPECT_EQ(5, x);
    EXPECT_PTR_EQ(&x, atri_test_return_alias(&x));
    EXPECT_EQ(17, atri_test_returned_pointer_write(&x));
    EXPECT_EQ(17, x);

    EXPECT_PTR_EQ(NULL, atri_test_null_pointer());
    EXPECT_PTR_EQ(&x, atri_test_address_of_dereference(&x));
    EXPECT_PTR_EQ(NULL, atri_test_address_of_dereference(NULL));
    EXPECT_EQ(0, atri_test_unused_call_result(&x));
    EXPECT_EQ(23, x);

    x = 5;
    y = 6;
    EXPECT_EQ(5, atri_test_reassign_pointer(&x, &y));
    EXPECT_EQ(11, y);
    return 0;
}
