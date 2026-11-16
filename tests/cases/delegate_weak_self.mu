// delegate_weak_self — `[weak this]` stores an uncounted reference, so a callback the object keeps on
// itself no longer forms an RC cycle: the object is destroyed and its destructor runs (U41).
unit delegate_weak_self;

use std.cstdio;

type Get = delegate () => int;

int live = 0;

class Counter {
    public Get cb;
    int n;
    Counter(int x) { n = x; live += 1; }
    ~Counter() { live -= 1; }
    void install() { cb = [weak this] () => n; }
}

int main() {
    {
        Counter c = new Counter(7);
        c.install();
        if live != 1 { printf("ctor\n"); return 1; }
        if c.cb() != 7 { printf("call while alive\n"); return 2; }
    }
    if live != 0 { printf("object leaked: the environment still owns it\n"); return 3; }
    printf("delegate weak self ok\n");
    return 0;
}
