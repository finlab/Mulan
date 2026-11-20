// return_move — `return x;` for a local or parameter of a movable-but-uncopyable type is a move.
// The name dies with the frame, so transferring it is safe; requiring `return move x` was pure
// syntax (SPEC 3.4). A copyable type keeps the copy path.
unit return_move;

use std.cstdio;

int moved = 0;
int copied = 0;
int freed = 0;

struct Owned {
    int* p;
    Owned(int n) { p = alloc(8) as int*; p[0] = n; }
    Owned(move Owned o) { moved = moved + 1; p = o.p; o.p = null; }
    ~Owned() {
        if (p != null) { freed = freed + 1; free(p); p = null; }
    }
}

struct Copyable {
    int v;
    Copyable(int n) { v = n; }
    Copyable(const ref Copyable o) { copied = copied + 1; v = o.v; }
}

Owned from_local(int n) {
    Owned b = Owned(n);
    return b;                 // implicit move
}

Owned from_param(Owned x) {
    return x;                 // the parameter is an owning local too
}

int copied_through(const ref Copyable c) {
    Copyable local = c;
    return local.v;
}

int main() {
    {
        Owned a = from_local(7);
        printf("%d %d\n", a.p[0], moved);
        Owned b = from_param(move a);
        printf("%d %d\n", b.p[0], moved);
    }
    // every allocation was released: the implicit moves transfer, they do not duplicate
    printf("freed %d\n", freed);

    Copyable src = Copyable(5);
    printf("copy %d\n", copied_through(src));
    printf("copied %d\n", copied);   // a copyable type still copies (one per call)
    return 0;
}
