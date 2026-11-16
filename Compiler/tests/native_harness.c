#include <stdint.h>
#include <stdio.h>

extern int32_t atri_fn_0(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
extern int32_t atri_fn_1(int32_t, int32_t);
extern int32_t atri_fn_2(int32_t, int32_t);
extern int32_t atri_fn_3(int32_t);
extern int32_t atri_fn_4(int32_t, int32_t);
extern int32_t atri_fn_5(int32_t);
#define EXPECT(call, value) do { if ((call) != (value)) { fputs(#call " failed\n", stderr); return 1; } } while (0)
int main(void) {
    EXPECT(atri_fn_0(1, 2, 3, 4, 5, 6), 21);
    EXPECT(atri_fn_1(-17, 5), -32);
    EXPECT(atri_fn_1(17, -5), -28);
    EXPECT(atri_fn_2(INT32_MAX, 1), INT32_MIN);
    EXPECT(atri_fn_2(INT32_MIN, -1), INT32_MAX);
    EXPECT(atri_fn_3(4), 20);
    EXPECT(atri_fn_3(-4), -4);
    EXPECT(atri_fn_4(-17, 5), -29);
    EXPECT(atri_fn_4(17, -5), -1);
    EXPECT(atri_fn_5(INT32_MIN), INT32_MIN);
    EXPECT(atri_fn_5(-7), 7);
    return 0;
}
