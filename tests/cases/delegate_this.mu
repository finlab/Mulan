// delegate_this — `[this]` captures the receiver (U41): a class captures the handle (the environment
// keeps the object alive), a struct captures a copy, and `[ref this]` on a struct stores its address so
// writes reach the original object.
unit delegate_this;

use std.cstdio;

type Get = delegate () => int;
type Bump = delegate () => void;

class Counter {
    public int n;
    Counter(int start) { n = start; }
    Get getter() { return [this] () => n; }
    Bump bumper() { return [this] () => { n = n + 1; }; }
}

struct Pair {
    public int a;
    public int b;
    Get sum() { return [this] () => a + b; }
}

struct Accum {
    public int total;
    Bump add(int by) { return [ref this, by] () => { total = total + by; }; }
    Get read() { return [ref this] () => total; }
}

int main() {
    // class: the handle is captured, so the lambda sees the live object and keeps it alive
    Counter c = new Counter(10);
    Get g = c.getter();
    Bump b = c.bumper();
    b();
    b();
    if g() != 12 { printf("class this\n"); return 1; }
    c = null;
    if g() != 12 { printf("class keepalive\n"); return 2; }

    // struct: a copy is captured, so later changes to the original are not visible
    Pair p = { a = 3, b = 4 };
    Get s = p.sum();
    p.a = 100;
    if s() != 7 { printf("struct copy\n"); return 3; }

    // `[ref this]` on a struct: writes reach the object the delegate was taken from
    Accum acc = { total = 0 };
    Bump add2 = acc.add(5);
    add2();
    add2();
    Get rd = acc.read();
    if rd() != 10 { printf("struct ref\n"); return 4; }
    if acc.total != 10 { printf("struct visible\n"); return 5; }

    printf("delegate this ok\n");
    return 0;
}
