unit sum_unit_eq;
use std.cstdio;

// Every alternative is a unit value, so equality is a tag comparison. The lowering used to emit an
// empty switch and then branch into the terminated block (invalid IR).
type Yes = A | B;

int main() {
    Yes a = A;
    Yes b = B;
    if a == b {
        printf("wrong: A == B\n");
        return 1;
    }
    if not (a != b) {
        printf("wrong: A != B false\n");
        return 2;
    }
    if b != Yes.B {
        printf("wrong: B != Yes.B\n");
        return 3;
    }
    if a == Yes.A {
        printf("tag ok\n");
    }
    return 0;
}
