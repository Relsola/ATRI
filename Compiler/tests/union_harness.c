#include "union_harness_bindings.h"
#include "native_expect.h"
#include <stdarg.h>

_Static_assert(sizeof(atri_test_U1) == 1 && _Alignof(atri_test_U1) == 1, "one byte union");
_Static_assert(sizeof(atri_test_U2) == 2 && _Alignof(atri_test_U2) == 2, "two byte union");
_Static_assert(sizeof(atri_test_U3) == 3 && _Alignof(atri_test_U3) == 1, "three byte union");
_Static_assert(sizeof(atri_test_U4) == 4 && _Alignof(atri_test_U4) == 4, "four byte union");
_Static_assert(sizeof(atri_test_U8) == 8 && _Alignof(atri_test_U8) == 8, "eight byte union");
_Static_assert(sizeof(atri_test_U12) == 12 && _Alignof(atri_test_U12) == 4, "twelve byte union");
_Static_assert(sizeof(atri_test_U16) == 16 && _Alignof(atri_test_U16) == 8, "rounded union size");
_Static_assert(offsetof(atri_test_U8, parts) == 0 && offsetof(atri_test_U8, pointer) == 0, "overlapping members");
_Static_assert(sizeof(atri_test_Envelope) == 32 && offsetof(atri_test_Envelope, value) == 8 &&
    offsetof(atri_test_Envelope, tail) == 24, "nested union layout");

atri_test_U1 c_echo1(atri_test_U1 v) { return v; }
atri_test_U2 c_echo2(atri_test_U2 v) { return v; }
atri_test_U3 c_echo3(atri_test_U3 v) { return v; }
atri_test_U4 c_echo4(atri_test_U4 v) { return v; }
atri_test_U8 c_echo8(atri_test_U8 v) { return v; }
atri_test_U12 c_echo12(atri_test_U12 v) { return v; }
atri_test_U16 c_echo16(atri_test_U16 v) { return v; }
atri_test_Envelope c_envelope(atri_test_Envelope v) { return v; }
uint64_t c_fifth(atri_test_U1 a, atri_test_U2 b, atri_test_U4 c, atri_test_U8 d, atri_test_U16 e, atri_test_U3 f) {
    return a.byte+b.word+c.word+d.wide+e.bytes[8]+f.bytes[2];
}
uint8_t c_independent(atri_test_U16 a, atri_test_U16 b) {
    a.bytes[0] = 99;
    return (uint8_t)(a.bytes[0]+b.bytes[0]);
}
uint64_t c_variadic(int32_t prefix, ...) {
    va_list args;
    va_start(args, prefix);
    atri_test_U4 a = va_arg(args, atri_test_U4);
    atri_test_U16 b = va_arg(args, atri_test_U16);
    va_end(args);
    return (uint64_t)prefix+a.word+b.bytes[8];
}
void c_modify(atri_test_U8 *p) { p->parts.low = UINT32_C(0x76543210); }

int main(void) {
    atri_test_U1 a = {.byte = 255};
    atri_test_U2 b = {.word = UINT16_C(0x9876)};
    atri_test_U3 c = {.bytes = {4,5,6}};
    atri_test_U4 d = {.word = UINT32_C(0x89abcdef)};
    atri_test_U8 e = {.wide = UINT64_C(0xfedcba9876543210)};
    atri_test_U12 f = {.words = {11,22,33}};
    atri_test_U16 g = {.bytes = {1,2,3,4,5,6,7,8,9}};
    EXPECT_EQ(a.byte, atri_test_echo1(a).byte); EXPECT_EQ(a.byte, atri_test_call_c1(a).byte);
    EXPECT_EQ(b.word, atri_test_echo2(b).word); EXPECT_EQ(b.word, atri_test_call_c2(b).word);
    atri_test_U3 c1 = atri_test_echo3(c), c2 = atri_test_call_c3(c);
    for (size_t i = 0; i < 3; ++i) { EXPECT_EQ(c.bytes[i], c1.bytes[i]); EXPECT_EQ(c.bytes[i], c2.bytes[i]); }
    EXPECT_EQ(d.word, atri_test_echo4(d).word); EXPECT_EQ(d.word, atri_test_call_c4(d).word);
    EXPECT_TRUE(e.wide == atri_test_echo8(e).wide); EXPECT_TRUE(e.wide == atri_test_call_c8(e).wide);
    atri_test_U12 f1 = atri_test_echo12(f), f2 = atri_test_call_c12(f);
    for (size_t i = 0; i < 3; ++i) { EXPECT_EQ(f.words[i], f1.words[i]); EXPECT_EQ(f.words[i], f2.words[i]); }
    atri_test_U16 g1 = atri_test_echo16(g), g2 = atri_test_call_c16(g);
    for (size_t i = 0; i < 9; ++i) { EXPECT_EQ(g.bytes[i], g1.bytes[i]); EXPECT_EQ(g.bytes[i], g2.bytes[i]); }
    EXPECT_TRUE(c_fifth(a,b,d,e,g,c) == atri_test_fifth(a,b,d,e,g,c));
    EXPECT_TRUE(c_fifth(a,b,d,e,g,c) == atri_test_call_c_fifth(a,b,d,e,g,c));
    EXPECT_EQ(100, atri_test_independent(g));
    EXPECT_EQ(1, g.bytes[0]);
    EXPECT_EQ(UINT64_C(10)+d.word+g.bytes[8], atri_test_variadic(d,g));

    atri_test_Envelope box = {.tag = 3, .value = g, .tail = 1234};
    atri_test_Envelope box1 = atri_test_echo_envelope(box), box2 = atri_test_call_c_envelope(box);
    EXPECT_EQ(3, box1.tag); EXPECT_EQ(3, box2.tag);
    EXPECT_EQ(1234, box1.tail); EXPECT_EQ(1234, box2.tail);
    for (size_t i = 0; i < 9; ++i) { EXPECT_EQ(g.bytes[i], box1.value.bytes[i]); EXPECT_EQ(g.bytes[i], box2.value.bytes[i]); }
    atri_test_Envelope initial = atri_test_global_big();
    EXPECT_EQ(1, initial.tag); EXPECT_EQ(99, initial.tail);
    for (size_t i = 0; i < 9; ++i) { EXPECT_EQ(i+1, initial.value.bytes[i]); }
    EXPECT_EQ(0, atri_test_zero_default());
    EXPECT_EQ(7, atri_test_selected_default());
    EXPECT_EQ(7, atri_test_local_defaults());
    EXPECT_TRUE(atri_test_raw_bits() == UINT64_MAX);
    EXPECT_PTR_EQ(NULL, atri_test_handle(NULL)); EXPECT_TRUE(atri_test_null_link());
    atri_test_U16 array[3] = {0};
    EXPECT_PTR_EQ(array+2, atri_test_offset(array,2));
    EXPECT_EQ(2, atri_test_distance(array+2,array));
    atri_test_U8 *global = atri_test_global_address();
    EXPECT_TRUE((uintptr_t)global % _Alignof(atri_test_U8) == 0);
    EXPECT_TRUE(global->wide == UINT64_C(0x0123456789abcdef));
    EXPECT_PTR_EQ(&global->parts.low, atri_test_low_address());
    atri_test_U8 snapshot = atri_test_global_value();
    *atri_test_low_address() = 42;
    EXPECT_EQ(42, atri_test_global_low());
    EXPECT_TRUE(snapshot.wide == UINT64_C(0x0123456789abcdef));
    EXPECT_EQ(UINT32_C(0x76543210), atri_test_modify());
    int32_t value = 23;
    global->pointer = &value;
    EXPECT_PTR_EQ(&value, atri_test_global_value().pointer);
    return 0;
}
