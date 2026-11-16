#include "c_abi_harness_bindings.h"
#include "native_expect.h"
#include <string.h>

static char c_storage[] = "C-owned text";

int64_t c_probe(char *text, int64_t wide, int32_t tag, int32_t *object, char *tail, int64_t last) {
    EXPECT_EQ(0, strcmp(text, "ATRI"));
    EXPECT_EQ(INT64_C(5000000007), wide);
    EXPECT_EQ(-7, tag);
    EXPECT_EQ(0, strcmp(tail, "tail"));
    EXPECT_EQ(INT64_C(-6000000009), last);
    *object = 42;
    return wide + last;
}

char *c_text(void) {
    return c_storage;
}

char *c_identity(char *text) {
    return text;
}

bool c_bool(int32_t value) {
    return value != 0;
}

int32_t c_bump(int32_t *object) {
    *object += 9;
    return *object;
}

int main(void) {
    int32_t object = 3;
    EXPECT_EQ(INT64_C(-1000000002), atri_test_call_c(&object, INT64_C(5000000007), INT64_C(-6000000009)));
    EXPECT_EQ(42, object);
    EXPECT_EQ(9, atri_test_observe_c_write(&object));
    EXPECT_EQ(51, object);
    EXPECT_PTR_EQ(c_storage, atri_test_from_c());
    EXPECT_PTR_EQ(c_storage, atri_test_round_trip(c_storage));
    EXPECT_PTR_EQ(NULL, atri_test_round_trip(NULL));
    EXPECT_TRUE(atri_test_negate_c_bool(0));
    EXPECT_FALSE(atri_test_negate_c_bool(-7));
    EXPECT_EQ(0, strcmp(atri_test_static_text(), "static text"));
    EXPECT_PTR_EQ(c_storage + 3, atri_test_char_offset(c_storage, 3));
    EXPECT_PTR_EQ(c_storage, atri_test_char_offset(c_storage + 3, -3));
    EXPECT_PTR_EQ(c_storage + 5, atri_test_char_offset32(c_storage, 5));
    EXPECT_PTR_EQ(c_storage, atri_test_char_offset32(c_storage + 5, -5));
    EXPECT_EQ(5, atri_test_char_distance(c_storage + 5, c_storage));
    EXPECT_EQ(-5, atri_test_char_distance(c_storage, c_storage + 5));
    EXPECT_PTR_EQ(NULL, atri_test_default_char_null());
    EXPECT_PTR_EQ(NULL, atri_test_choose_text(false));
    EXPECT_EQ(0, strcmp(atri_test_choose_text(true), "chosen"));
    return 0;
}
