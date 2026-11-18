// nocopy_ctorless_expr — a `nocopy` struct without a constructor is still built by `T(args)` /
// `T(field = v)`: construction is not copying (SPEC 3.4/3.13). Borrowing the value afterwards is the
// sanctioned way to read it.
unit nocopy_ctorless_expr;

use std.cstdio;

nocopy struct Token {
    public int v;
    public int tag;
}

int take(const ref Token t) {
    return t.v + t.tag;
}

int main() {
    Token a = Token(5, 1);              // positional construction
    if a.v != 5 or a.tag != 1 { printf("positional\n"); return 1; }

    Token b = Token(tag = 2, v = 6);    // named construction, any field order
    if b.v != 6 or b.tag != 2 { printf("named\n"); return 2; }

    if take(a) != 6 { printf("borrow\n"); return 3; }

    printf("nocopy ctorless ok\n");
    return 0;
}
