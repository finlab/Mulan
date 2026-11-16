// iface_mutable_indexable.mu — read-only and read-write index contracts
unit iface_mutable_indexable;

extern int puts(const char* s);

// one operator, two capabilities: `ref int` serves both contracts
struct Buf {
    int[] d;

    isize length() { return d.length(); }
    ref int operator[](isize i) { return d[i]; }
}

// a read-only container: `int` (no borrow)
struct Ro {
    int[] d;

    isize length() { return d.length(); }
    int operator[](isize i) { return d[i]; }
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
    Buf b = {};
    b.d = new int[3];
    fill<Buf>(b, 9);
    if b.d[0] != 9 or b.d[2] != 9 { puts("fill"); return 1; }
    if total<Buf>(b) != 27 { puts("read after write"); return 2; }

    // the same contract also accepts a built-in block, which is mutable
    int[] raw = new int[2];
    fill<int[]>(raw, 4);
    if raw[1] != 4 { puts("block fill"); return 3; }
    if total<int[]>(raw) != 8 { puts("block read"); return 4; }

    // a read-only container satisfies the read contract
    Ro r = {};
    r.d = new int[2];
    r.d[1] = 5;
    if total<Ro>(r) != 5 { puts("read-only"); return 5; }

    puts("iface mutable indexable ok");
    return 0;
}
