// para_for_basic — `para for` over a range, one task per item: each iteration writes a disjoint
// element through a class handle (explicit sharing), then a sequential sum checks the result.

unit para_for_basic;

use std.cstdio;

class Buf {
    public int[16] v;
}

int main() {
    Buf b = new Buf();
    int n = 16;
    para for i in 0..n {
        b.v[i] = i * i;
    }
    int sum = 0;
    for k in 0..n {
        sum = sum + b.v[k];
    }
    printf("%d\n", sum);
    return 0;
}
