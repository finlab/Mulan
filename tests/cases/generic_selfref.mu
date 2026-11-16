unit generic_selfref;
use std.cstdio;

// A generic may refer to the instance being defined through a pointer or a borrow (a C-style
// incomplete type): `Node<T>* next`, and the copy/move constructors of `Box<T>`.
struct Node<T> {
    public T v;
    public Node<T>* next;
}

struct Box<T> {
    public T v;
    Box(T x) { v = x; }
    Box(const ref Box<T> o) { v = o.v; }
    Box(move Box<T> o) { v = o.v; }
}

int main() {
    Node<int> n3 = { 3, null };
    Node<int> n2 = { 2, &n3 };
    Node<int> n1 = { 1, &n2 };
    int sum = 0;
    Node<int>* p = &n1;
    while p != null {
        sum += p->v;
        p = p->next;
    }
    if sum != 6 {
        printf("bad list sum %d\n", sum);
        return 1;
    }
    Box<int> a = Box<int>(5);
    Box<int> b = a;
    Box<int> c = move a;
    if b.v != 5 or c.v != 5 {
        printf("bad box\n");
        return 2;
    }
    printf("selfref ok\n");
    return 0;
}
