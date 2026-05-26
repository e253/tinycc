#include <stdio.h>

static int __attribute__((enum_callconv)) add(int a, int b) { return a + b; }
static int __attribute__((enum_callconv)) mul(int a, int b) { return a * b; }

static int __attribute__((enum_callconv)) compute(int x, int y, int z) {
    int s = add(x, y);   /* call site 0 for add */
    int p = mul(s, z);   /* call site 0 for mul */
    return p;
}

int main(void) {
    int a = add(3, 4);         /* call site 1 for add */
    int b = mul(2, 5);         /* call site 1 for mul */
    int c = compute(1, 2, 3);  /* call site 0 for compute */
    printf("add=%d mul=%d compute=%d\n", a, b, c);
    /* expected: add=7 mul=10 compute=9 */
    return !(a == 7 && b == 10 && c == 9);
}
