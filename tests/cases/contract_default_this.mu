// `this` in an interface default body is the implementor (SPEC 3.15.6): a contract operator applies to
// it, so `this[0]` works. Such a default needs the concrete implementor, so it is materialized per
// implementor (struct and class) and takes no itable slot.
unit contract_default_this;
use std.cstdio;

interface IIndex<T> {
    isize length();
    T at(isize i);
    T operator[](Self c, isize i);
    T first() { return this[0]; }
    T last() { return this.at(this.length() - 1); }
}

struct Arr : IIndex<int> {
    public int[4] d;
    public isize n;
    Arr() { n = 3; d[0] = 7; d[1] = 8; d[2] = 9; }
    override isize length() { return n; }
    override int at(isize i) { return d[i]; }
    override int operator[](isize i) { return d[i]; }
}

class Vec : IIndex<int> {
    public int[4] d;
    public isize n;
    Vec() { n = 3; d[0] = 7; d[1] = 8; d[2] = 9; }
    override isize length() { return this.n; }
    override int at(isize i) { return this.d[i]; }
    override int operator[](isize i) { return this.d[i]; }
}

int ends<T: IIndex<int>>(T c) { return c.first() + c.last(); }

int main() {
    Arr a;
    Vec v = new Vec();
    if a.first() != 7 or a.last() != 9 { printf("struct default failed\n"); return 1; }
    if v.first() != 7 or v.last() != 9 { printf("class default failed\n"); return 2; }
    if ends<Arr>(a) != 16 { printf("struct generic failed\n"); return 3; }
    if ends<Vec>(v) != 16 { printf("class generic failed\n"); return 4; }
    printf("default this ok\n");
    return 0;
}
