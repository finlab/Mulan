// exception_boundaries — errors must cross a function boundary in all the shapes the may-throw
// analysis used to miss: runtime bounds, a copy constructor in assignment, a move constructor in an
// argument, a constructor init-list in an argument, an array element's default constructor and a
// cursor's next().
unit exception_boundaries;

extern int puts(const char* s);

class Boom : Exception {
    Boom() { }
}

int caught = 0;

int get(int[] a, int i) { return a[i]; }

struct S {
    public int v;
    S(int x) { v = x; }
    S(const ref S o) {
        v = o.v;
        if o.v == 666 { throw new Boom(); }
    }
}

struct Buf {
    int n;
    Buf(int k) { n = k; }
    Buf(move Buf o) {
        n = o.n;
        if o.n == 42 { throw new Boom(); }
    }
}

Buf make_buf() {
    Buf b = Buf(42);
    return move b;
}

void take_buf(Buf b) { }

class Obj {
    public int v;
    Obj(int x) {
        if x == 0 { throw new Boom(); }
        v = x;
    }
}

class Holder {
    public Obj o;
    Holder() : o(new Obj(0)) { }
}

void take_holder(Holder h) { }

struct Elem {
    public int n;
    Elem() {
        n = 1;
        throw new Boom();
    }
}

void make_elem_array() {
    Elem[3] a;
}

struct ThrowCursor {
    public int i;
    ThrowCursor() { i = 0; }
    bool next() {
        i += 1;
        if i == 2 { throw new Boom(); }
        return i <= 3;
    }
    int current() { return i; }
}

class Src {
    ThrowCursor iterator() { return ThrowCursor(); }
}

void drive_cursor() {
    Src s = new Src();
    for r in s { }
}

int main() {
    {
        int[] a = new int[3];
        try { int v = get(a, 3); } catch (e: Exception) { caught += 1; }
    }
    try {
        S x = S(666);
        S y = S(1);
        y = x;
    } catch (e: Exception) { caught += 1; }

    try { take_buf(make_buf()); } catch (e: Exception) { caught += 1; }
    try { take_holder(new Holder()); } catch (e: Exception) { caught += 1; }
    try { make_elem_array(); } catch (e: Exception) { caught += 1; }
    try { drive_cursor(); } catch (e: Exception) { caught += 1; }

    if caught != 6 { puts("exception boundary lost"); return 1; }
    puts("exception boundaries ok");
    return 0;
}
