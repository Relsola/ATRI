#include "multi_level_pointers_harness_bindings.h"
#include "native_expect.h"

void c_replace(uint8_t **slot, uint8_t *value) { *slot = value; }
uint8_t **c_echo(uint8_t **slot) { return slot; }

int main(void) {
    uint8_t a = 17, b = 42;
    uint8_t *slots[] = {&a, &a};
    EXPECT_EQ(42, atri_test_retarget(&b));
    uint8_t **slot = &slots[0];
    EXPECT_EQ(42, atri_test_triple(&slot, &b));
    EXPECT_PTR_EQ(&b, slots[0]);
    EXPECT_EQ(1, atri_test_swap_next(slots, &b));
    EXPECT_PTR_EQ(&b, slots[1]);
    EXPECT_PTR_EQ(&slots[0], atri_test_via_c(&slots[0], &a));
    EXPECT_PTR_EQ(&a, slots[0]);
    EXPECT_PTR_EQ(&slots[1], atri_test_cast_roundtrip(&slots[1]));
    atri_test_Opaque *opaque = NULL;
    EXPECT_PTR_EQ(NULL, atri_test_opaque_slot(&opaque));
    atri_test_Link link = {0};
    EXPECT_PTR_EQ(&link.target, atri_test_link_slot(&link));
    EXPECT_TRUE(atri_test_global_is_null());
    uint8_t **global = atri_test_global_slot();
    EXPECT_PTR_EQ(NULL, *global);
    *global = &a;
    EXPECT_PTR_EQ(&a, *atri_test_global_slot());
    EXPECT_EQ(2 * sizeof(void *), atri_test_layout());
    EXPECT_EQ(42, atri_test_array_targets(&b));
    return 0;
}
