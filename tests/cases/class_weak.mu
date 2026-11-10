unit class_weak;

extern int puts(const char* s);

class Node {
    int v;
    weak Node next;      // 非持有引用
    Node(int x) { v = x; next = null; }
    ~Node() { }
    int get() { return v; }
    void link(Node n) { next = n; }
    Node peek() { return next; }
}

int main() {
    Node a = new Node(1);
    Node b = new Node(2);
    Node keep = b;         // keep 是强引用
    a.link(b);             // weak 不增加计数
    del b;
    if keep.get() != 2 { puts("keep failed"); return 1; }
    if a.peek().get() != 2 { puts("weak read failed"); return 1; }
    del keep;              // 目标销毁时 weak 字段会被自动置 null
    del a;
    puts("class weak ok");
    return 0;
}
