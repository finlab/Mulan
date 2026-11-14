// para_for_ref — `for ref x in a` with one task per item: the producer passes the element address
// in the per-item context and the task writes through it (the container outlives the region).

unit para_for_ref;

use std.cstdio;

int main() {
    int[] a = {0, 1, 2, 3, 4, 5, 6, 7};
    para for ref v in a {
        v = v * 2;
    }
    int sum = 0;
    for k in 0..8 {
        sum = sum + a[k];
    }
    printf("%d\n", sum);
    return 0;
}
