// generic_infer_deep — argument deferral (the argument needs a parameter type), a later argument or
// the target type supplying the parameter, partial explicit + target, and literal widening.
unit generic_infer_deep;

use std.option;

extern int puts(const char* s);

T first_like<T>(Option<T> o, T fallback) {
    return match move o { Some(v): v; None: fallback; };
}

struct Pair<A, B> { A a; B b; }
Pair<T, U> make_pair<T, U>() {
    Pair<T, U> p = {};
    return p;
}

struct Box<T> { T v; }
Box<T> make_box<T>() {
    Box<T> b = {};
    return b;
}

T id<T>(T x) { return x; }

int main() {
    // `Some(7)` cannot be typed on its own; T comes from the second argument.
    int a = first_like(Some(7), 0);
    if a != 7 { puts("deferred/later arg"); return 1; }

    // Partial explicit + target: T is written, U comes from Pair<int, string>.
    Pair<int, string> p = make_pair<int>();
    if p.a != 0 { puts("partial/target"); return 2; }

    Option<int> oi = Some(9);
    int b = first_like(oi, 0);
    if b != 9 { puts("option arg"); return 3; }

    // The literal binds T=int; the result widens to long (the target does not override arguments).
    long y = id(1);
    if y != 1 { puts("widening"); return 4; }

    Box<int> bx = make_box();
    if bx.v != 0 { puts("target"); return 5; }

    puts("generic infer deep ok");
    return 0;
}
