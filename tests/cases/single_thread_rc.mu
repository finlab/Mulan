unit single_thread_rc;

use std.cstdio;

// Same object graph as `class_weak`, compiled with `--single-thread`: reference counting must stay
// correct without the atomics (retain across a call, array release, weak auto-nulling).
class Node {
    int v;
    weak Node next;
    Node(int x) { v = x; next = null; }
    ~Node() { }
    int get() { return v; }
    void link(Node n) { next = n; }
    Node peek() { return next; }
}

int read(Node n) {
    return n.get();
}

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    Node keep = b;
    a.link(b);
    b = null;
    if read(a) != 1 { printf("read failed\n"); return 1; }
    if keep.get() != 2 { printf("keep failed\n"); return 1; }
    if a.peek().get() != 2 { printf("weak read failed\n"); return 1; }

    int[] arr = new int[4];
    arr[0] = 7;
    int sum = arr[0];
    arr = null;

    keep = null;
    a = null;
    printf("single-thread rc ok %d\n", sum);
    return 0;
}
