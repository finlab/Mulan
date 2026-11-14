// delegate_basic — the `delegate` type (two words `{code, env}`). This build has the type, the calling
// convention and the `fn` -> `delegate` conversion; capturing closures (a non-null environment) land
// in the next step, so every environment here is null.
unit delegate_basic;

use std.cstdio;

type Op = delegate (int, int) => int;

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

struct Calc {
    public Op op;
}

int apply(Op f, int a, int b) {
    return f(a, b);
}

Op pick(bool use_mul) {
    if use_mul { return mul; }
    return add;
}

int main() {
    // a free function is a `fn` value; assigning it to a `delegate` goes through a thunk that drops
    // the unused environment
    Op add_op = add;
    if add_op(2, 3) != 5 { printf("fn->delegate\n"); return 1; }

    // a non-capturing lambda converts the same way
    Op scale = (int a, int b) => a * b + 1;
    if scale(2, 3) != 7 { printf("lambda->delegate\n"); return 2; }

    // a delegate is a value: field, argument, return value
    Calc c = { op = mul };
    if c.op(4, 5) != 20 { printf("field\n"); return 3; }
    if apply(add_op, 7, 8) != 15 { printf("argument\n"); return 4; }
    if pick(true)(3, 3) != 9 { printf("return\n"); return 5; }

    // layout: two words regardless of the signature
    Op one = add_op;
    if (sizeof(one) as int) != 16 { printf("sizeof\n"); return 6; }

    // null is the empty delegate
    Op empty = null;
    if sizeof(empty) as int != 16 { printf("null sizeof\n"); return 7; }

    printf("delegate ok\n");
    return 0;
}
