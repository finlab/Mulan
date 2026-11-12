unit class_weak;

extern int puts(const char* s);

class Node {
    int v;
    weak Node next;      // non-owning reference
    Node(int x) { v = x; next = null; }
    ~Node() { }
    int get() { return v; }
    void link(Node n) { next = n; }
    Node peek() { return next; }
}

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    Node keep = b;         // keep holds a strong reference
    a.link(b);             // weak does not bump the count
    b = null;
    if keep.get() != 2 { puts("keep failed"); return 1; }
    if a.peek().get() != 2 { puts("weak read failed"); return 1; }
    keep = null; // weak field is auto-nulled when the target dies
    a = null;
    puts("class weak ok");
    return 0;
}
