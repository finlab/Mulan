// layout.mu — layout modifiers packed / align(N)
unit layout_case;

extern int puts(const char* s);
extern int putchar(int c);

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

void nl() { putchar('\n'); }

// packed: fields pack back to back, no internal/trailing padding
packed struct W {
    byte a;
    int b;
    ushort c;
}

// align(16): field offsets unchanged; only alignof is raised to 16 and sizeof padded to a multiple of 16
align(16) struct V {
    int x, y;
}

// order independent, composable
packed align(8) struct M {
    byte a;
    int b;
    ushort c;
}

// written the other way (align first) is equivalent
align(4) packed struct R {
    byte a;
    int b;
}

// a packed struct used as a field of a plain struct keeps its own tight layout
struct Q {
    byte pad;
    W w;
}

// align(16) field inside a plain struct: the field offset is 16-aligned
struct Outer {
    byte lead;
    V v;
    int tail;
}

int main() {
    // ---------- packed sizeof / alignof / offsets ----------
    print_int(sizeof(W) as int); putchar(' ');
    print_int(alignof(W) as int); putchar(' ');
    W w = {a = 0x12, b = 0x12345678, c = 0xABCD};
    print_int((((&w.a) as usize) - ((&w) as usize)) as int); putchar(' ');
    print_int((((&w.b) as usize) - ((&w) as usize)) as int); putchar(' ');
    print_int((((&w.c) as usize) - ((&w) as usize)) as int); nl();  // 7 1 0 1 5

    // ---------- Byte-level layout (little-endian) and read/write round trip ----------
    byte* bp = (&w) as byte*;
    print_int(bp[0] as int); putchar(' ');
    print_int(bp[1] as int); putchar(' ');
    print_int(bp[4] as int); putchar(' ');
    print_int(bp[5] as int); putchar(' ');
    print_int(bp[6] as int); nl();  // 18 120 18 205 171
    w.a = 0x18;
    w.c = 0x1234;
    print_int(w.a as int); putchar(' ');
    print_int(w.b); putchar(' ');
    print_int(w.c as int); nl();  // 24 305419896 4660

    // ---------- align(16) ----------
    V v = {1, 2};
    print_int(sizeof(V) as int); putchar(' ');
    print_int(alignof(V) as int); putchar(' ');
    print_int((((&v) as usize) % 16) as int); putchar(' ');
    print_int(v.x + v.y); nl();  // 16 16 0 3

    // ---------- packed align(8) ----------
    M m = {a = 0x7F, b = -2, c = 3};
    print_int(sizeof(M) as int); putchar(' ');
    print_int(alignof(M) as int); putchar(' ');
    print_int((((&m) as usize) % 8) as int); putchar(' ');
    print_int((((&m.b) as usize) - ((&m) as usize)) as int); putchar(' ');
    print_int((((&m.c) as usize) - ((&m) as usize)) as int); putchar(' ');
    print_int(m.b); nl();  // 8 8 0 1 5 -2

    // ---------- order independence + packed struct as a plain struct field ----------
    R r = {a = 1, b = -5};
    print_int(sizeof(R) as int); putchar(' ');
    print_int(alignof(R) as int); putchar(' ');
    print_int(((&r.b) as usize - ((&r) as usize)) as int); putchar(' ');
    print_int(r.b); nl();  // 8 4 1 -5

    Q q;
    q.pad = 7;
    q.w.a = 1;
    q.w.b = 99;
    print_int(sizeof(Q) as int); putchar(' ');
    print_int(((&q.w) as usize - ((&q) as usize)) as int); putchar(' ');
    print_int(((&q.w.b) as usize - ((&q) as usize)) as int); putchar(' ');
    print_int(q.w.b); nl();  // 8 1 2 99

    // ---------- Array element stride ----------
    W[3] ws;
    print_int((((&ws[1]) as usize) - ((&ws[0]) as usize)) as int); putchar(' ');
    print_int(sizeof(W[3]) as int); putchar(' ');
    V[2] vs;
    vs[0].x = 10;
    vs[1].x = 20;
    print_int((((&vs[1]) as usize) - ((&vs[0]) as usize)) as int); putchar(' ');
    print_int((((&vs[1]) as usize) % 16) as int); putchar(' ');
    print_int(vs[1].x); nl();  // 7 21 16 0 20

    // ---------- Nesting: plain struct with an align(16) field ----------
    Outer o;
    o.lead = 5;
    o.v.x = 6;
    o.tail = 7;
    print_int(sizeof(Outer) as int); putchar(' ');
    print_int(alignof(Outer) as int); putchar(' ');
    print_int((((&o.v) as usize) - ((&o) as usize)) as int); putchar(' ');
    print_int((((&o.tail) as usize) - ((&o) as usize)) as int); putchar(' ');
    print_int(o.lead as int + o.v.x + o.tail); nl();  // 48 16 16 32 18

    if sizeof(W) == 7 and alignof(W) == 1 and sizeof(V) == 16 and alignof(V) == 16 and
        sizeof(M) == 8 and alignof(M) == 8 {
        puts("layout ok");
    }
    return 0;
}
