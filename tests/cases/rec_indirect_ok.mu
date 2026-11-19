// Indirect recursion is fine as long as the cycle is broken by a pointer or a class handle: the layout
// stays finite. The recursive-layout check must not reject these.
unit rec_indirect_ok;

use std.cstdio;

struct Node {
    int v;
    Node* next;
}

class Link {
    public int v;
    public Link next;
    public Link(int k) { v = k; }
}

T* skip<T>(T* p) { return p; }

int main() {
    Node a;
    a.v = 1;
    Node b;
    b.v = 2;
    b.next = null;
    a.next = &b;
    printf("%d %d\n", a.v, a.next->v);

    Link l = new Link(7);
    l.next = null;
    printf("%d\n", l.v);
    return 0;
}
