#include <stdint.h>
#include <stdio.h>

extern int32_t atri_fn_0(int32_t *, int32_t);
extern int32_t atri_fn_1(int32_t *, int32_t *);
extern int32_t *atri_fn_2(int32_t *);
extern int32_t atri_fn_3(int32_t);
extern int32_t atri_fn_4(int32_t, int32_t *, int32_t, int32_t *, int32_t *, int32_t);
extern int32_t atri_fn_5(int32_t *);
extern int32_t *atri_fn_6(int32_t *);
extern int32_t atri_fn_7(int32_t *);
extern int32_t *atri_fn_8(void);
extern int32_t *atri_fn_9(int32_t *);
extern int32_t atri_fn_10(int32_t *);
extern int32_t atri_fn_11(int32_t *, int32_t *);
#define EXPECT(c) do { if (!(c)) { fputs(#c " failed\n", stderr); return 1; } } while (0)
int main(void) {
    int32_t x = 1, y = 2, z = 3;
    EXPECT(atri_fn_0(&x, 42) == 42 && x == 42);
    EXPECT(atri_fn_1(&x, &x) == 7 && x == 7);
    EXPECT(atri_fn_1(&x, &y) == 5 && x == 5 && y == 4);
    EXPECT(atri_fn_2(&x) == &x);
    EXPECT(atri_fn_3(7) == 10);
    EXPECT(atri_fn_3(INT32_MAX) == INT32_MIN + 2);
    EXPECT(atri_fn_4(10, &x, 20, &y, &z, 30) == 60);
    EXPECT(x == 10 && y == 20 && z == 30);
    x = 1;
    EXPECT(atri_fn_5(&x) == 10 && x == 5);
    EXPECT(atri_fn_6(&x) == &x);
    EXPECT(atri_fn_7(&x) == 17 && x == 17);
    EXPECT(atri_fn_8() == NULL);
    EXPECT(atri_fn_9(&x) == &x && atri_fn_9(NULL) == NULL);
    EXPECT(atri_fn_10(&x) == 0 && x == 23);
    x = 5; y = 6;
    EXPECT(atri_fn_11(&x, &y) == 5 && y == 11);
    return 0;
}
