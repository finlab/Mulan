// arrow_basic — `p->f` is exactly `(*p).f`: reads, writes, chained pointers, array members through a
// pointer, and a pointer parameter used through `->`.

unit arrow_basic;

use std.cstdio;

struct Node {
    Node* next;
    int v;
    int[3] arr;
}

struct Pair {
    int a;
    int b;
}

int sum_pair(Pair* p) { return p->a + p->b; }

int main() {
    Pair p = { a = 1, b = 2 };
    Pair* q = &p;
    q->a = 10;
    q->b += 5;
    printf("%d %d\n", p.a, p.b);

    Node n2 = { next = null, v = 7, arr = {4, 5, 6} };
    Node n1 = { next = &n2, v = 3, arr = {1, 2, 3} };
    printf("%d %d %d\n", n1.next->v, n1.next->arr[1], (*n1.next).arr[2]);

    Node* np = &n1;
    np->v = np->v + 100;
    printf("%d %d\n", n1.v, sum_pair(q));
    return 0;
}
