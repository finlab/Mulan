unit class_basic;

extern int puts(const char* s);

int live = 0;

class Counter {
    int v;
    Counter(int start) { v = start; live += 1; }
    ~Counter() { live -= 1; }
    int get() { return v; }
    void set(int next) { v = next; }
}

int main() {
    Counter a = new Counter(7);
    if a.get() != 7 { puts("ctor failed"); return 1; }
    if live != 1 { puts("live failed"); return 1; }

    Counter b = a;               // 共享强引用
    if live != 1 { puts("share failed"); return 1; }
    a = null; // 只减少一份引用
    if b.get() != 7 { puts("share get failed"); return 1; }
    if live != 1 { puts("premature dtor"); return 1; }

    b.set(9);
    if b.get() != 9 { puts("set failed"); return 1; }

    Counter c = new Counter(1);
    c = b;                       // 重绑定：旧对象析构，新对象 +1
    if live != 1 { puts("rebind failed"); return 1; }

    b = null;
    if c.get() != 9 { puts("rebind get failed"); return 1; }
    if live != 1 { puts("early dtor"); return 1; }

    c = null;
    if live != 0 { puts("dtor failed"); return 1; }

    puts("class basic ok");
    return 0;
}
