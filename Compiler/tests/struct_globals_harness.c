#include "struct_globals_harness_bindings.h"
#include "native_expect.h"

void c_modify(atri_test_Packet *value) {
    value->inner.count = 42;
    value->rows[1].count = 77;
    value->bytes[2] = 255;
}
static void expect_defaults(const atri_test_Packet *value) {
    EXPECT_EQ('A', value->tag);
    EXPECT_EQ(-32768, value->negative);
    EXPECT_EQ(UINT32_C(0x89abcdef), value->number);
    EXPECT_TRUE(value->wide == UINT64_MAX);
    EXPECT_EQ(8, value->inner.count); EXPECT_TRUE(value->inner.ready);
    EXPECT_EQ(0, value->bytes[0]); EXPECT_EQ(0, value->bytes[2]);
    EXPECT_EQ(8, value->rows[0].count); EXPECT_TRUE(value->rows[0].ready);
    EXPECT_EQ(8, value->rows[1].count); EXPECT_TRUE(value->rows[1].ready);
    EXPECT_PTR_EQ(NULL, value->link);
}
int main(void) {
    _Static_assert(sizeof(atri_test_Packet) == 56 && _Alignof(atri_test_Packet) == 8,
        "global packet target layout");
    atri_test_Packet *defaults = atri_test_defaults_address();
    atri_test_Packet *configured = atri_test_configured_address();
    EXPECT_TRUE((uintptr_t)defaults % _Alignof(atri_test_Packet) == 0);
    expect_defaults(defaults);
    EXPECT_EQ('B', configured->tag);
    EXPECT_EQ(9, configured->inner.count); EXPECT_TRUE(configured->inner.ready);
    EXPECT_EQ(1, configured->bytes[0]); EXPECT_EQ(2, configured->bytes[1]); EXPECT_EQ(0, configured->bytes[2]);
    EXPECT_EQ(2, configured->rows[0].count); EXPECT_TRUE(configured->rows[0].ready);
    EXPECT_EQ(8, configured->rows[1].count);
    EXPECT_PTR_EQ(NULL, configured->link);
    atri_test_Packet *zeroed = atri_test_zeroed_address();
    EXPECT_EQ(0, zeroed->wide); EXPECT_EQ(0, zeroed->inner.count); EXPECT_FALSE(zeroed->inner.ready);
    EXPECT_EQ(8, zeroed->rows[0].count);
    EXPECT_EQ(255, atri_test_tiny_address()->value);

    atri_test_Packet snapshot = atri_test_defaults_value();
    expect_defaults(&snapshot);
    EXPECT_PTR_EQ(&defaults->inner.count, atri_test_count_address());
    EXPECT_PTR_EQ(&defaults->rows[1], atri_test_row_address());
    *atri_test_count_address() = 12;
    EXPECT_EQ(12, atri_test_count());
    EXPECT_EQ(8, snapshot.inner.count);
    EXPECT_EQ(42, atri_test_modify_from_c());
    EXPECT_EQ(77, defaults->rows[1].count); EXPECT_EQ(255, defaults->bytes[2]);
    EXPECT_EQ(9, configured->inner.count);

    snapshot.inner.count = -9;
    snapshot.link = configured;
    atri_test_Packet result = atri_test_assign(snapshot);
    EXPECT_EQ(-9, result.inner.count); EXPECT_EQ(-9, defaults->inner.count);
    EXPECT_PTR_EQ(configured, result.link); EXPECT_PTR_EQ(configured, defaults->link);
    EXPECT_TRUE(atri_test_guard_value() == UINT64_C(0x0123456789abcdef));
    return 0;
}
