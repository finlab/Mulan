// generic_struct_iface — a generic struct implements interfaces like any value type: a generic
// interface (IShow<T>), a default method materialized per instance (IVal), and a generic algorithm
// constrained by IIndex<T>.
unit generic_struct_iface;

use std.interfaces;

extern int puts(const char* s);

interface IShow<T> { T get(); }

struct Box<T> : IShow<T> {
    T v;
    Box() {}
    T get() { return v; }
}

interface IVal {
    int val();
    int twice() { return val() * 2; }
}

struct G<T> : IVal {
    T v;
    G() {}
    int val() { return 1; }
}

struct Row<T> : IIndex<T> {
    T[] d;
    Row() {}
    isize length() { return d.length(); }
    T operator[](isize i) { return d[i]; }
}

int total<T: IIndex<int>>(T c) {
    int s = 0;
    for i in 0..c.length() { s += c[i]; }
    return s;
}

int main() {
    Box<int> b = {};
    b.v = 5;
    if b.get() != 5 { puts("generic iface"); return 1; }

    G<int> g = {};
    if g.twice() != 2 { puts("default"); return 2; }

    Row<int> r = {};
    r.d = new int[3];
    r.d[0] = 1; r.d[1] = 2; r.d[2] = 3;
    if total(r) != 6 { puts("index constraint"); return 3; }

    puts("generic struct iface ok");
    return 0;
}
