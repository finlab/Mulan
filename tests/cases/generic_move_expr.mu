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

int main() {
    Box<int> box = new Box<int>(42);
    if box.get() != 42 { printf("ctor move\n"); return 1; }

    if pass_through<int>(7) != 7 { printf("func move\n"); return 2; }

    int x = 1;
    int y = 2;
    exchange<int>(x, y);
    if x != 2 or y != 1 { printf("generic swap\n"); return 3; }

    printf("generic move expr ok\n");
    return 0;
}
