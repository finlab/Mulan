// layout2.mu — class 对象内的 align(N) / packed 字段，以及动态数组元素对齐
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

int main() {
    // ---------- class 字段：对象基址 16 对齐，align(16) 字段按模型对齐 ----------
    C c = new C();
    c.a.x = 11;
    c.p.a = 90;
    c.p.b = 123456;
    print_int((((&c.t) as usize) % 16) as int); putchar(' ');
    print_int((((&c.a) as usize) % 16) as int); putchar(' ');
    print_int((((&c.a) as usize) - ((&c.t) as usize)) as int); putchar(' ');
    print_int((((&c.p) as usize) - ((&c.t) as usize)) as int); putchar(' ');
    print_int((((&c.p.b) as usize) - ((&c.p) as usize)) as int); nl();  // 0 0 16 32 1

    // ---------- 方法内隐式 this 访问 packed 字段 ----------
    c.setb(-7);
    print_int(c.ax()); putchar(' ');
    print_int(c.pb()); putchar(' ');
    print_int(c.p.a as int); nl();  // 11 -7 90

    // ---------- 定长数组仍允许 align(16) ----------
    Al[2] st;
    st[0].x = 1;
    st[1].x = 2;
    print_int((((&st[0]) as usize) % 16) as int); putchar(' ');
    print_int((((&st[1]) as usize) - ((&st[0]) as usize)) as int); nl();  // 0 16

    // ---------- 动态数组元素最多 8 字节对齐：align(8) 正常 ----------
    A8[] s = new A8[2];
    s[0].a = 5;
    s[1].a = 6;
    print_int((((&s[0]) as usize) % 8) as int); putchar(' ');
    print_int((((&s[1]) as usize) - ((&s[0]) as usize)) as int); putchar(' ');
    print_int(s[0].a + s[1].a); nl();  // 0 8 11

    // ---------- align(32) 字段：对象用对齐分配（_aligned_malloc） ----------
    D d = new D();
    d.t = 1;
    d.w.x = 2;
    print_int((((&d.w) as usize) % 32) as int); putchar(' ');
    print_int((((&d.w) as usize) - ((&d.t) as usize)) as int); putchar(' ');
    print_int(d.t as int + d.w.x); nl();  // 0 16 3
    d = null;

    if ((((&c.t) as usize) % 16) == 0 and (((&c.a) as usize) % 16) == 0 and
        ((((&c.a) as usize) - ((&c.t) as usize)) == 16) and
        ((((&c.p.b) as usize) - ((&c.p) as usize)) == 1) and
        ((((&st[0]) as usize) % 16) == 0) and (((&s[0]) as usize) % 8) == 0) {
        puts("layout2 ok");
    }
    return 0;
}
