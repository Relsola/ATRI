#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
extern int32_t atri_fn_0(int32_t);
extern int32_t atri_fn_1(int32_t);
extern int32_t atri_fn_2(int32_t);
extern int32_t atri_fn_3(int32_t);
extern int32_t atri_fn_4(int32_t);
extern int32_t atri_fn_5(int32_t);
extern int32_t atri_fn_6(int32_t);
extern int32_t atri_fn_7(int32_t);
extern int32_t atri_fn_8(int32_t *, int32_t *);
extern bool atri_fn_9(int32_t *);
extern bool atri_fn_10(bool, bool, bool, bool, bool, bool);
extern int32_t atri_fn_12(void);
extern int32_t atri_fn_13(void);
extern int64_t atri_fn_14(int64_t);
extern int32_t atri_fn_15(void);
extern bool atri_fn_16(int32_t);
extern bool atri_fn_17(int32_t);
extern bool atri_fn_18(int64_t, int64_t);
extern bool atri_fn_19(int32_t *);
extern int32_t atri_fn_20(int32_t);
extern bool atri_fn_21(void);
int main(void) {
    CHECK(atri_fn_0(-2) == -1 && atri_fn_0(0) == 0 && atri_fn_0(4) == 1);
    CHECK(atri_fn_1(20) == 25 && atri_fn_1(0) == 0);
    CHECK(atri_fn_2(5) == 8 && atri_fn_2(0) == 0);
    CHECK(atri_fn_3(0) == 1 && atri_fn_3(5) == 5);
    CHECK(atri_fn_4(4) == 4);
    CHECK(atri_fn_5(4) == 4 && atri_fn_6(4) == 4);
    CHECK(atri_fn_7(5) == 120);
    int32_t values[] = {1,2,3,4};
    CHECK(atri_fn_8(values, values+4) == 10 && atri_fn_8(values, values) == 0);
    CHECK(atri_fn_9(NULL) && !atri_fn_9(values));
    CHECK(atri_fn_10(true,false,true,false,true,true));
    CHECK(!atri_fn_10(true,false,true,false,true,false));
    CHECK(!atri_fn_10(true,false,true,true,true,true));
    CHECK(atri_fn_12() == 2);
    CHECK(atri_fn_13() == 5);
    CHECK(atri_fn_14(3) == INT64_C(15000000000));
    CHECK(atri_fn_15() == 163);
    CHECK(!atri_fn_16(0) && atri_fn_16(2) && !atri_fn_16(10));
    CHECK(atri_fn_17(4));
    CHECK(atri_fn_18(INT64_MIN, INT64_MAX) && !atri_fn_18(INT64_MAX, INT64_MIN));
    CHECK(!atri_fn_19(NULL) && atri_fn_19(values));
    CHECK(atri_fn_20(-1) == 5 && atri_fn_20(0) == 7);
    CHECK(atri_fn_21());
    return 0;
}
