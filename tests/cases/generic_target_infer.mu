// generic_target_infer — the expected type fills a type parameter that no argument determines:
// initialization, assignment, return, call argument, ternary, aggregate field, variant payload and
// slice element. Arguments win over the target; a convertible result is left to the assignment check.
unit generic_target_infer;

use std.option;

extern int puts(const char* s);

struct Box<T> { T v; }

Box<T> make<T>() {
    Box<T> b = {};
    return b;
}

Box<T> pass<T>(Box<T> b) { return b; }

Box<int> make_int() { return make(); }

T identity<T>(T x) { return x; }

T* pick<T>(T* p) { return p; }

int plus1(int x) { return x + 1; }
long plus1(long x) { return x + 2; }
T apply<T>((T) => T g, T x) { return g(x); }

struct Holder { Box<int> b; }

int main() {
    Box<int> a = make();
    a.v = 7;
    Box<int> b;
    b = make();
    Box<int> c = pass(a);
    Box<int> d = make_int();
    if c.v != 7 { puts("init/argument"); return 1; }
    if b.v != 0 { puts("assignment"); return 2; }
    if d.v != 0 { puts("return"); return 3; }

    // Arguments win: the literal gives T=int and the result widens to long.
    long y = identity(1);
    if y != 1 { puts("widening"); return 4; }

    // `null` has no type of its own; the target `int*` binds T and the argument check accepts null.
    int* p = pick(null);
    if p != null { puts("null"); return 5; }

    // An overloaded function name is deferred until the second argument binds T.
    int z = apply(plus1, 3);
    if z != 4 { puts("overload"); return 6; }

    // Other target positions.
    Box<int> t = true ? make() : make();
    Holder h = { b = make() };
    Option<Box<int>> o = Some(make());
    Box<int>[] xs = { make(), make() };
    if t.v != 0 or h.b.v != 0 { puts("ternary/field"); return 7; }
    if o is None { puts("variant"); return 8; }
    if xs.length() != 2 { puts("slice"); return 9; }

    puts("generic target infer ok");
    return 0;
}
