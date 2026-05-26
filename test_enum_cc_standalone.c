/* Standalone test: no libc needed. Uses syscall for exit. */

/* satisfy __stack_chk_fail reference from jae instruction */
void __stack_chk_fail(void) {
    /* Should never be reached in correct code. Infinite loop to signal failure. */
    while (1) {}
}

static int __attribute__((enum_callconv)) add(int a, int b) { return a + b; }
static int __attribute__((enum_callconv)) mul(int a, int b) { return a * b; }
static int __attribute__((enum_callconv)) compute(int x, int y, int z) {
    int s = add(x, y);
    int p = mul(s, z);
    return p;
}

int main(void) {
    int a = add(3, 4);
    int b = mul(2, 5);
    int c = compute(1, 2, 3);
    if (a != 7) return 1;
    if (b != 10) return 2;
    if (c != 9) return 3;
    return 0;
}
