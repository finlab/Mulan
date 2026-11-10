unit class_extra;

extern int puts(const char* s);

interface I {
    int get();
}

class C : I {
    int v;
    C(int x) { v = x; }
    ~C() { }
    override int get() { return v; }
}

class Bag {
    I[2] items;
    Bag() { items[0] = null; items[1] = null; }
    ~Bag() { }
}

C g = null;

I make(int x) { return new C(x); }

int main() {
    // 接口作为返回值
    I i = make(5);
    if i.get() != 5 { puts("iface return failed"); return 1; }
    i = null; // 接口数组
    I[2] arr = {};
    arr[0] = new C(1);
    arr[1] = new C(2);
    if arr[0].get() + arr[1].get() != 3 { puts("iface array failed"); return 1; }
    arr[0] = null;
    if arr[0] != null { puts("iface null failed"); return 1; }
    if arr[1].get() != 2 { puts("iface array retain failed"); return 1; }

    // 接口数组字段（析构时释放）
    Bag b = new Bag();
    b.items[0] = arr[1];
    b.items[1] = make(7);
    if b.items[1].get() != 7 { puts("iface field failed"); return 1; }
    b = null; // 全局 class 引用
    if g != null { puts("global init failed"); return 1; }
    g = new C(3);
    if g.get() != 3 { puts("global assign failed"); return 1; }
    g = null;
    if g != null { puts("global null failed"); return 1; }

    puts("class extra ok");
    return 0;
}
