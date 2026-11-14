// para_many_tasks — the M:N scheduler keeps "one task per item" cheap: thousands of tasks fan out
// over the carriers, each writing a disjoint slice element, and the region joins them all.

unit para_many_tasks;

use std.cstdio;

int main() {
    int[] v = new int[2000];
    para for i in 0..2000 {
        v[i] = i;
    }
    int s = 0;
    for k in 0..2000 {
        s = s + v[k];
    }
    printf("%d\n", s);
    v = null;
    return 0;
}
