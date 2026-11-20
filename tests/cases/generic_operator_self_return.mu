// generic_operator_self_return — a constraint operator returning `Self` yields T in the body; the
// definition-time witness check must leave the T-to-int conversion to the per-instance check.
unit generic_operator_self_return;

use std.cstdio;

interface IAdd {
    Self operator+(Self a, Self b);
}

int g<T: IAdd>(T a, T b) {
    return a + b;
}

int main() {
    if g(1, 2) != 3 { printf("g\n"); return 1; }
    printf("generic operator self return ok\n");
    return 0;
}
