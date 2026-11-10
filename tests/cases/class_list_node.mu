unit class_list_node;
extern int puts(const char* s);
int live = 0;
class Node {
    int v;
    Node next;
    weak Node prev;
    Node(int x) { v = x; next = null; prev = null; live += 1; }
    ~Node() { live -= 1; }
    int get() { return v; }
    void append(Node n) { next = n; n.prev = this; }
}
int main() {
    Node head = new Node(0);
    Node cur = head;
    for i in 1..5 {
        Node n = new Node(i);
        cur.append(n);
        cur = n;
    }
    if live != 5 { puts("live build failed"); return 1; }

    int sum = 0;
    Node walk = head;
    while walk != null {
        sum += walk.get();
        walk = walk.next;
    }
    if sum != 10 { puts("traverse failed"); return 1; }

    {
        Node back = head.next;
        if back.prev.get() != 0 { puts("weak prev failed"); return 1; }
    }

    cur = null;
    while head != null {
        Node nxt = head.next;
        head = null;
        head = nxt;
    }
    if live != 0 { puts("list cleanup failed"); return 1; }
    puts("class list ok");
    return 0;
}



