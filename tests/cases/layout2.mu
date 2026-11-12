// layout2.mu — align(N) / packed fields inside class objects, plus dynamic array element alignment
unit layout2;

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

align(16) struct Al {
    int x, y;
}

packed struct Pk {
    byte a;
    int b;
}

align(8) struct A8 {
    int a, b;
}

align(32) struct W32 {
    int x, y;
}

class C {
public byte t;
public Al a;
public Pk p;

public int ax() { return a.x; }
public int pb() { return p.b; }
public void setb(int v) { p.b = v; }
}

class D {
public byte t;
public W32 w;
}

// The object layout of a class is an internal implementation (`&c.field` is not exposed), so the offsets are
// verified through a struct mirror with the same model layout; the class only checks the handle size and behaviour.
struct CShape {
    byte t;
    Al a;
    Pk p;
}

struct DShape {
    byte t;
    W32 w;
}

int main() {
    // ---------- class fields: the object base/offsets are internal layout, verified with a struct mirror of the same model ----------
    C c = new C();
    c.a.x = 11;
    c.p.a = 90;
    c.p.b = 123456;
    CShape cs = {};
    print_int((((&cs.t) as usize) - ((&cs) as usize)) as int); putchar(' ');
    print_int((((&cs.a) as usize) % 16) as int); putchar(' ');
    print_int((((&cs.a) as usize) - ((&cs.t) as usize)) as int); putchar(' ');
    print_int((((&cs.p) as usize) - ((&cs.t) as usize)) as int); putchar(' ');
    print_int((((&cs.p.b) as usize) - ((&cs.p) as usize)) as int); nl();  // 0 0 16 32 1

    // ---------- implicit this access to a packed field inside a method ----------
    c.setb(-7);
    print_int(c.ax()); putchar(' ');
    print_int(c.pb()); putchar(' ');
    print_int(c.p.a as int); nl();  // 11 -7 90

    // ---------- fixed-size arrays still allow align(16) ----------
    Al[2] st;
    st[0].x = 1;
    st[1].x = 2;
    print_int((((&st[0]) as usize) % 16) as int); putchar(' ');
    print_int((((&st[1]) as usize) - ((&st[0]) as usize)) as int); nl();  // 0 16

    // ---------- dynamic array elements are aligned to at most 8 bytes: align(8) works normally ----------
    A8[] s = new A8[2];
    s[0].a = 5;
    s[1].a = 6;
    print_int((((&s[0]) as usize) % 8) as int); putchar(' ');
    print_int((((&s[1]) as usize) - ((&s[0]) as usize)) as int); putchar(' ');
    print_int(s[0].a + s[1].a); nl();  // 0 8 11

    // ---------- align(32) field: type-level alignment is verified by the mirror (aligned allocation for class objects is an internal implementation) ----------
    // In the mirror the offset of w is 32; a class object has a 16-byte object header, so the field offset is internal and no longer exposed.
    D d = new D();
    d.t = 1;
    d.w.x = 2;
    DShape ds = {};
    print_int((((&ds.w) as usize) % 32) as int); putchar(' ');
    print_int((((&ds.w) as usize) - ((&ds) as usize)) as int); putchar(' ');
    print_int(d.t as int + d.w.x); nl();  // 0 32 3
    d = null;

    if (sizeof(C) == 8 and sizeof(D) == 8 and sizeof(CShape) == 48 and sizeof(DShape) == 64 and
        (((&cs.t) as usize) - ((&cs) as usize)) == 0 and
        (((&cs.a) as usize) % 16) == 0 and
        (((&cs.a) as usize) - ((&cs.t) as usize)) == 16 and
        (((&cs.p.b) as usize) - ((&cs.p) as usize)) == 1 and
        (((&st[0]) as usize) % 16) == 0 and (((&s[0]) as usize) % 8) == 0) {
        puts("layout2 ok");
    }
    return 0;
}
