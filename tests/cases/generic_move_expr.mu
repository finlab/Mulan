// generic_move_expr — a generic instantiation clones the whole body, including `move` and `swap`.
// `move` names the source local of the original body, so the clone must not carry that pointer: the
// instantiated body has its own parameter and local bindings and sema resolves the move again.
unit generic_move_expr;

use std.cstdio;

class Box<T> {
    public T data;
    Box(move T value) { data = move value; }
    T get() { return data; }
}

T pass_through<T>(T v) {
    T out = move v;
    return move out;
}

void exchange<T>(ref T a, ref T b) {
    swap(a, b);
}

// a class generic's *method* body is cloned too, and `swap` may appear there
class Pair<T> {
    T first;
    T second;

    Pair(T a, T b) {
        first = move a;
        second = move b;
    }

    void flip() {
        swap(first, second);
    }

    T head() { return first; }
    T tail() { return second; }
}

// a move-only element type proves the instantiated body transfers ownership instead of copying
nocopy struct Buf {
    int* p;
    Buf(int n) { p = alloc(n) as int*; }
    Buf(move Buf other) { p = other.p; }
    ~Buf() { free(p); }
}

T own_up<T>(T v) {
    T held = move v;
    return move held;
}

int main() {
    Box<int> box = new Box<int>(42);
    if box.get() != 42 { printf("ctor move\n"); return 1; }

    if pass_through<int>(7) != 7 { printf("func move\n"); return 2; }

    int x = 1;
    int y = 2;
    exchange<int>(x, y);
    if x != 2 or y != 1 { printf("generic swap\n"); return 3; }

    Pair<int> p = new Pair<int>(10, 20);
    if p.head() != 10 or p.tail() != 20 { printf("pair init\n"); return 4; }
    p.flip();
    if p.head() != 20 or p.tail() != 10 { printf("method swap\n"); return 5; }

    {
        Buf b = Buf(8);
        Buf c = own_up<Buf>(move b);
        if c.p == null { printf("owned buffer\n"); return 6; }
    }

    printf("generic move expr ok\n");
    return 0;
}
