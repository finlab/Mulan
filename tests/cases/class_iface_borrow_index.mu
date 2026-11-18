// class_iface_borrow_index — `ref T operator[]` satisfies a declared `T operator[]` in a class's
// nominal `override`, the same rule the structural path applies (a borrow is usable as a value,
// StdLibSPEC 6.2): one operator serves both `IIndex` (read) and `IIndexMut` (write).
unit class_iface_borrow_index;

use std.cstdio;
use std.interfaces;

class Buf : IIndexMut<int> {
    public int[] d;
    Buf(isize n) {
        d = new int[n];
        for i in 0..n { d[i] = (i * 10) as int; }
    }
    override isize length() { return d.length(); }
    override ref int operator[](isize i) { return d[i]; }
}

int total<T: IIndex<int>>(T c) {
    int s = 0;
    for i in 0..c.length() { s += c[i]; }
    return s;
}

void fill<T: IIndexMut<int>>(T c, int v) {
    for ref x in c { x = v; }
}

int main() {
    Buf b = new Buf(3);
    if total<Buf>(b) != 30 { printf("total\n"); return 1; }

    fill<Buf>(b, 7);
    if b[0] != 7 or b[2] != 7 { printf("write through\n"); return 2; }
    if b[1] != 7 { printf("write through 1\n"); return 3; }

    printf("borrow index ok\n");
    return 0;
}
