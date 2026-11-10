unit param_ownership;

// 参数是拥有型局部变量（SPEC §4）：进入 retain、退出 release；
// 函数体内重绑定参数不得影响调用方，也不能泄漏。
// for-in 值绑定同此处理（每轮 retain/release，覆盖 break/continue/return）。

extern int puts(const char* s);

int live = 0;

class C {
    int v;
    C(int n) { v = n; live += 1; }
    ~C() { live -= 1; }
    int get() { return v; }
}

struct S { C c; int x; }

interface I { int get(); }

class D : I {
    int v;
    D(int n) { v = n; live += 1; }
    ~D() { live -= 1; }
    override int get() { return v; }
}

class Base {
    C b;
    Base(C x) { b = x; }
    int bget() { return b.v; }
}

class Derived : Base {
    Derived(C x) { super(x); }
}

class Holder {
    C c;
    Holder(C x) { c = x; }
    int take(C p) { C q = p; q = null; return p.v; }
    int reassign(C p) { p = new C(77); return p.v; }
}

void drop(C c) { c = null; }
void rebind(C c) { c = new C(9); }
C ident(C c) { return c; }
void takeS(S s) { if s.x < 0 { } }
void takeI(I i) { if i.get() < 0 { } }

int find(C[] arr, int want) {
    for c in arr {
        if c.v == want { return c.v; }
    }
    return -1;
}

int main() {
    // 1. 参数置 null 不释放调用方对象
    {
        C a = new C(1);
        drop(a);
        if live != 1 { puts("param null over-release"); return 1; }
        if a.v != 1 { puts("param null use-after-free"); return 2; }
    }
    if live != 0 { puts("param null leak"); return 3; }

    // 2. 参数赋 owned 值：不释放调用方，也不泄漏
    {
        C a = new C(1);
        rebind(a);
        if live != 1 { puts("param rebind leak"); return 4; }
        if a.v != 1 { puts("param rebind corrupted caller"); return 5; }
    }
    if live != 0 { puts("param rebind leak 2"); return 6; }

    // 3. 返回参数：调用方拿到一份新引用
    {
        C a = new C(2);
        C b = ident(a);
        if live != 1 { puts("return param count"); return 7; }
        if b.v != 2 { puts("return param value"); return 8; }
        b = null;
        if live != 1 { puts("return param release"); return 9; }
    }
    if live != 0 { puts("return param leak"); return 10; }

    // 4. 临时实参
    drop(new C(3));
    if live != 0 { puts("temp arg leak"); return 11; }

    // 5. 含引用字段的 struct 参数
    {
        S s = { new C(4), 1 };
        takeS(s);
        if live != 1 { puts("struct param rc"); return 12; }
        if s.c.v != 4 { puts("struct param corrupted"); return 13; }
    }
    if live != 0 { puts("struct param leak"); return 14; }

    // 6. 接口参数
    {
        D d = new D(5);
        takeI(d);
        if live != 1 { puts("iface param rc"); return 15; }
        if d.get() != 5 { puts("iface param corrupted"); return 16; }
    }
    if live != 0 { puts("iface param leak"); return 17; }

    // 7. 方法参数重绑定
    {
        C a = new C(6);
        Holder h = new Holder(a);
        if h.take(a) != 6 { puts("method param value"); return 18; }
        if h.reassign(a) != 77 { puts("method reassign value"); return 19; }
        if live != 1 { puts("method param rc"); return 20; }
        if a.v != 6 { puts("method reassign corrupted caller"); return 21; }
        h = null;
    }
    if live != 0 { puts("method leak"); return 22; }

    // 8. 构造器 / super(...) 参数
    {
        C a = new C(7);
        Derived d = new Derived(a);
        if live != 1 { puts("super param rc"); return 23; }
        if d.bget() != 7 { puts("super param value"); return 24; }
        d = null;
        if live != 1 { puts("super param release"); return 25; }
    }
    if live != 0 { puts("super param leak"); return 26; }

    // 9. for-in 值绑定可重绑定，且每轮释放
    {
        C[] arr = new C[3];
        arr[0] = new C(10);
        arr[1] = new C(11);
        arr[2] = new C(12);
        int sum = 0;
        for c in arr {
            sum += c.v;
            c = new C(99);
            if live != 4 { puts("loopvar rebind rc"); return 27; }
        }
        if live != 3 { puts("loopvar leak"); return 28; }
        if sum != 33 { puts("loopvar value"); return 29; }
        arr = null;
        if live != 0 { puts("arr release"); return 30; }
    }

    // 10. break / continue / return 路径上的循环变量释放
    {
        C[] arr = new C[3];
        arr[0] = new C(20);
        arr[1] = new C(21);
        arr[2] = new C(22);
        int n = 0;
        for c in arr {
            n += 1;
            if n == 1 { continue; }
            if n == 2 { break; }
        }
        if live != 3 { puts("loop break/continue leak"); return 31; }
        if find(arr, 22) != 22 { puts("find value"); return 32; }
        if live != 3 { puts("find leak"); return 33; }
        arr = null;
        if live != 0 { puts("loop break/continue release"); return 34; }
    }

    puts("param ownership ok");
    return 0;
}
