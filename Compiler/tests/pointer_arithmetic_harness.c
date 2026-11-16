#include <stdint.h>
#include <stdio.h>
extern int32_t *atri_fn_0(int32_t *, int32_t);
extern int32_t *atri_fn_1(int32_t, int32_t *);
extern int32_t *atri_fn_2(int32_t *, int32_t);
extern int32_t *atri_fn_3(int32_t *, int64_t);
extern int32_t *atri_fn_4(int32_t *, int64_t);
extern int64_t atri_fn_5(int32_t *, int32_t *);
extern int32_t atri_fn_6(int32_t *, int32_t);
extern int32_t atri_fn_7(int32_t *, int32_t, int32_t);
extern int32_t *atri_fn_8(int32_t *, int64_t);
extern int64_t atri_fn_9(int32_t *, int64_t);
extern int32_t *atri_fn_10(int32_t *, int32_t);
extern int32_t *atri_fn_11(int32_t *);
extern int64_t atri_fn_14(int64_t, int64_t);
extern int64_t atri_fn_15(int64_t, int32_t, int32_t *, int64_t, int64_t, int32_t *);
extern int64_t atri_fn_16(void);
extern int64_t atri_fn_17(int64_t);
#define EXPECT(c) do { if (!(c)) { fputs(#c " failed\n", stderr); return 1; } } while (0)
int main(void) {
    int32_t a[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    EXPECT(atri_fn_0(a, 3) == a + 3);
    EXPECT(atri_fn_0(a + 4, -3) == a + 1);
    EXPECT(atri_fn_1(-2, a + 4) == a + 2);
    EXPECT(atri_fn_2(a + 4, 3) == a + 1);
    EXPECT(atri_fn_2(a + 4, -2) == a + 6);
    EXPECT(atri_fn_3(a, 8) == a + 8);
    EXPECT(atri_fn_3(a + 4, -3) == a + 1);
    EXPECT(atri_fn_4(a + 8, 8) == a);
    EXPECT(atri_fn_4(a + 4, -2) == a + 6);
    EXPECT(atri_fn_5(a + 8, a) == 8);
    EXPECT(atri_fn_5(a + 1, a + 7) == -6);
    EXPECT(atri_fn_5(a, a) == 0);
    EXPECT(atri_fn_6(a + 4, -2) == 30);
    EXPECT(atri_fn_7(a, 3, 99) == 99 && a[3] == 99);
    EXPECT(atri_fn_8(a, 8) == a);
    EXPECT(atri_fn_9(a + 4, -3) == -3);
    EXPECT(atri_fn_10(a + 4, -1) == a + 3);
    EXPECT(atri_fn_11(a) == a && a[0] == 5);
    EXPECT(atri_fn_14(INT64_C(5000000000), 7) == INT64_C(-7500000001));
    EXPECT(atri_fn_14(INT64_C(-5000000000), 7) == INT64_C(7499999994));
    EXPECT(atri_fn_15(INT64_C(5000000000), 1, a + 1, INT64_C(6000000000), INT64_C(7000000000), a + 5) == INT64_C(18000000004));
    EXPECT(atri_fn_16() == INT64_C(10000000001));
    EXPECT(atri_fn_17(INT64_MAX) == INT64_MIN);
    return 0;
}
