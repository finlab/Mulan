unit class_weak_clear;
extern int puts(const char* s);
int live = 0;
class Node {
    int v;
    weak Node next;
    Node(int x) { v = x; next = null; live += 1; }
    ~Node() { live -= 1; }
    int get() { return v; }
    void link(Node n) { next = n; }
    bool has_next() { return next != null; }
    Node peek() { return next; }
}
class Holder {
    weak Node n;
    Holder(Node x) { n = x; }
    ~Holder() { }
    bool has() { return n != null; }
}
int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    a.link(b);
    if a.peek() != b { puts("weak link fail"); return 1; }
    b = null;
    if a.next != null { puts("weak not cleared on destroy"); return 1; }
    if a.has_next() { puts("weak has_next fail"); return 1; }

    Node c = new Node(3);
    Holder h = new Holder(c);
    if not h.has() { puts("holder weak fail"); return 1; }
    c = null;
    if h.n != null { puts("holder weak not cleared"); return 1; }
    h = null;

    Node d = new Node(4);
    Node e = new Node(5);
    Node f = new Node(6);
    d.link(e);
    d.link(f);                  // overwrite the weak target
    e = null;
    if d.next != f { puts("weak overwrite stale null"); return 1; }
    f = null;
    if d.next != null { puts("weak overwrite clear fail"); return 1; }

    Node g = new Node(7);
    g.link(g);                  // self-referencing weak
    g = null;

    a = null; d = null;
    if live != 0 { puts("weak live fail"); return 1; }
    puts("weak clear ok");
    return 0;
}

