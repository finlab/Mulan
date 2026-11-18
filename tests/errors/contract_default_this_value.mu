// A default whose body reads `this` needs the implementor type, so it has no itable slot: calling it
// through an interface value is a compile error. The other members stay dispatchable.
unit contract_default_this_value;
use std.cstdio;

interface IIndex<T> {
    isize length();
    T at(isize i);
    T operator[](Self c, isize i);
    T first() { return this[0]; }
}

class Vec : IIndex<int> {
    public int[4] d;
    public isize n;
    Vec() { n = 2; d[0] = 1; d[1] = 2; }
    override isize length() { return this.n; }
    override int at(isize i) { return this.d[i]; }
    override int operator[](isize i) { return this.d[i]; }
}

int main() {
    Vec v = new Vec();
    IIndex<int> i = v;
    isize n = i.length();          // fine: no `this` in the body, dispatched through the itable
    return i.first() + n;        // error: the default reads `this`
}
