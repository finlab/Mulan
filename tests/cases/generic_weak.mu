// generic_weak — a `weak` field inside a generic class stays weak after instantiation: it neither
// keeps the target alive nor is left dangling when the holder dies first.
unit generic_weak;

use std.cstdio;

int live = 0;

class Node {
    public int id;
    Node(int i) { id = i; live += 1; }
    ~Node() { live -= 1; }
}

class Holder<T> {
    public weak Node peer;
    public T payload;
    Holder() { }
}

int main() {
    Holder<int> h = new Holder<int>();
    Node n = new Node(7);
    h.payload = 3;
    h.peer = n;
    if h.peer != n { printf("weak read\n"); return 1; }
    if h.peer.id != 7 { printf("weak member\n"); return 2; }

    // the weak field must not hold a reference
    n = null;
    if live != 0 { printf("the weak field kept the target alive\n"); return 3; }
    if h.peer != null { printf("the weak field was not cleared\n"); return 4; }

    // a second holder, this time dropped before the target: the registration is unregistered
    Node m = new Node(8);
    h.peer = m;
    Holder<int> h2 = h;
    h2 = null;
    h = null;
    if live != 1 { printf("holder release\n"); return 5; }
    m = null;
    if live != 0 { printf("target release after the holder died first\n"); return 6; }

    printf("generic weak ok\n");
    return 0;
}
