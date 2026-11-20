// generic_system_matrix — the supported generic combinations: class + interface defaults, class
// inheritance, interface inheritance, a constrained generic struct, a generic union, a unit-level
// generic instance, an interface value of a generic class, partial explicit + target, and a
// target-bound parameter with a target-typed argument.
unit generic_system_matrix;

use std.cstdio;
use std.option;

interface IShow<T> {
    T get();
    int twice() { return 2; }
}

class C<T> : IShow<T> {
    public T v;
    C() {}
    override T get() { return v; }
}

class B<T> {
    public T v;
    B() {}
    T get() { return v; }
}
class D<T> : B<T> {
    D() {}
}

interface IBase<T> { T get(); }
interface IDerived<T> : IBase<T> { T twice(); }

struct S<T> : IDerived<T> {
    T v;
    S() {}
    T get() { return v; }
    T twice() { return v; }
}

interface IAdd { Self operator+(Self a, Self b); }

struct Box<T: IAdd> {
    T v;
    Box() {}
    T sum(T x) { return v + x; }
}

union U<T> { T x; long y; }

struct Pair<A, B> { A a; B b; }
Pair<T, U> make_pair<T, U>() {
    Pair<T, U> p = {};
    return p;
}

T id<T>(T x) { return x; }

Box<int> G = {};

int main() {
    C<int> c = new C<int>();
    c.v = 6;
    IShow<int> i = c;
    if i.get() != 6 or i.twice() != 2 { printf("class iface\n"); return 1; }

    D<int> d = new D<int>();
    d.v = 8;
    if d.get() != 8 { printf("class inherit\n"); return 2; }

    S<int> s = {};
    s.v = 4;
    if s.get() != 4 or s.twice() != 4 { printf("iface inherit\n"); return 3; }

    Box<int> b = {};
    b.v = 3;
    if b.sum(4) != 7 { printf("constraint\n"); return 4; }

    U<int> u = {};
    u.x = 1;
    if u.x != 1 { printf("union\n"); return 5; }

    G.v = 2;
    if G.v != 2 { printf("unit var\n"); return 6; }

    Pair<int, string> p = make_pair<int>();
    if p.a != 0 { printf("partial target\n"); return 7; }

    Option<int> o = id(Some(3));
    if o is None { printf("target arg\n"); return 8; }

    printf("generic system matrix ok\n");
    return 0;
}
