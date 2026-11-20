// zero_value — `T()` is the zero value of a type (SPEC 4): a primitive (`int()`, `bool()`, `double()`),
// a struct (zeroed, its member step runs when it needs one) or a **type parameter** inside a generic
// body. This is the same value `{}` produces at a declaration.
unit zero_value;

use std.cstdio;

struct P {
    int a;
    int* q;
}

class C { }

T zero_of<T>() {
    return T();
}

int main() {
    printf("%d %d %d\n", int(), bool() ? 1 : 0, double() == 0.0 ? 1 : 0);

    P p = P();
    printf("%d %d\n", p.a, p.q == null);

    // a type parameter: the value depends on the instantiation
    printf("%d %d\n", zero_of<int>(), zero_of<long>());
    P q = zero_of<P>();
    printf("%d %d\n", q.a, q.q == null);
    C c = zero_of<C>();
    printf("%d\n", c == null ? 1 : 0);
    return 0;
}
