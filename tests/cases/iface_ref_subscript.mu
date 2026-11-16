// iface_ref_subscript.mu — a constraint may declare a `ref` subscript, so a generic algorithm can
// write through the elements. The container is passed by value: the handle is shared, so element
// writes are visible to the caller and no `ref` borrow of the container is needed.
unit iface_ref_subscript;

extern int puts(const char* s);

interface IMutable<T> {
    isize length();
    ref T operator[](Self c, isize i);
}

struct Buf {
    int[] d;

    isize length() { return d.length(); }
    ref int operator[](isize i) { return d[i]; }
}

void fill<T: IMutable<int>>(T c, int v) {
    for i in 0..c.length() {
        c[i] = v;
    }
}

int main() {
    Buf b = {};
    b.d = new int[3];
    fill<Buf>(b, 7);
    if b.d[0] != 7 or b.d[2] != 7 { puts("ref subscript write failed"); return 1; }

    puts("iface ref subscript ok");
    return 0;
}
