// variant_is_logic — an `is` test **without** a binding is an ordinary `bool` expression, so it
// composes with `and` / `or` / `not` and may be used in any condition position (if, while, the
// condition of a C-style for, and the `?:` selector). SPEC 10.1.1.
//
// The binding form (`is Some(v)`) is the exception: it must be the whole condition, because the bound
// name only exists inside that branch (must-fail case `is_bind_compose`).
unit variant_is_logic;

use std.option;
use std.cstdio;

class Animal { }
class Cat : Animal { }

int main() {
    Option<int> a = Some(1);
    Option<int> b = None;
    bool flag = true;

    int r = 0;
    if a is Some and flag { r = r + 1; }
    if b is None and flag { r = r + 2; }
    if a is None or flag { r = r + 4; }
    if b is Some or flag { r = r + 8; }
    bool both = (a is Some) and (b is None);
    if both { r = r + 16; }
    if not (a is None) { r = r + 32; }
    printf("%d\n", r);

    // the `?:` selector and a loop condition take the same expression
    int pick = (a is Some and flag) ? 7 : 0;
    int n = 0;
    while a is Some and n < 2 { n = n + 1; }
    int m = 0;
    for (; a is Some and m < 3; ) { m = m + 1; }
    printf("%d %d %d\n", pick, n, m);

    // the same operator tests a class handle against a subtype, with no binding
    Animal an = new Cat();
    if an is Cat and flag { printf("cat\n"); }
    return 0;
}
