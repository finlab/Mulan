// para_for_array — `para for v in a` over a fixed-size array: the producer walks the indices
// through the ordinary for-in lowering and starts one task per item, passing the element value in
// the per-item context.

unit para_for_array;

use std.cstdio;

class Out {
    public int[8] r;
}

int f(int x) { return x * 3; }

int main() {
    int[8] a = {0, 1, 2, 3, 4, 5, 6, 7};
    Out o = new Out();
    para for k in 0..8 {
        o.r[k] = f(a[k]);
    }
    int s = 0;
    for k in 0..8 {
        s = s + o.r[k];
    }
    printf("%d\n", s);
    return 0;
}
