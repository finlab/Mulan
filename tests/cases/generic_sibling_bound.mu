// generic_sibling_bound — a bound may name a sibling type parameter (`T: IIter<U>, U`): the
// declaration no longer reports "unknown type U", and the bound is checked with the actual arguments.
unit generic_sibling_bound;

use std.cstdio;

interface IIter<U> {
    U next();
}

struct It : IIter<int> {
    int v;
    It() { v = 7; }
    int next() { return v; }
}

U first<T: IIter<U>, U>(T it) {
    return it.next();
}

int main() {
    It i = {};
    int x = first(i);
    if x != 7 { printf("x=%d\n", x); return 1; }
    printf("generic sibling bound ok\n");
    return 0;
}
