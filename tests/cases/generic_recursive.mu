// generic_recursive — an instance can refer to itself through a class handle, a self pointer, a
// mutually recursive pair of generic structs (via pointers), and a recursive variant.
unit generic_recursive;

extern int puts(const char* s);

class N<T> {
    public N<T> next;
    public T v;
    N() {}
}

struct A<T> {
    B<T>* b;
    T v;
    A() {}
}

struct B<T> {
    A<T>* a;
    T v;
    B() {}
}

variant Tree<T> {
    Leaf T;
    Node Tree<T>*;
}

int main() {
    N<int> n = new N<int>();
    n.next = n;
    n.v = 1;
    if n.next.v != 1 { puts("class"); return 1; }

    A<int> a = {};
    B<int> b = {};
    a.b = &b;
    b.a = &a;
    a.v = 3;
    b.v = 4;
    if a.b->v + b.a->v != 7 { puts("mutual"); return 2; }

    Tree<int> t = Leaf(5);
    if not (t is Leaf) { puts("variant"); return 3; }

    puts("generic recursive ok");
    return 0;
}
