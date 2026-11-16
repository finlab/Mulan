// move_return — a movable-but-uncopyable value crosses a function boundary: `return move x;` and
// `f(move x)` are the only ways to hand it over (agents/value-semantics.md §8.3).
unit move_return;

extern int printf(const char* fmt, ...);

int moves = 0;

nocopy struct Buf {
    int* p;
    Buf(int n) { p = alloc(n) as int*; }
    ~Buf() { free(p); }
    Buf(move Buf o) { p = o.p; moves = moves + 1; }
}

Buf make(int n) {
    Buf b = Buf(n);
    return move b;          // `b` is out of reach after this line
}

int take(Buf b) {           // the parameter owns what it was given
    int ok = 0;
    if b.p != null { ok = 1; }
    return ok;
}

int main() {
    Buf a = make(8);
    if a.p == null { printf("return move\n"); return 1; }

    Buf c = Buf(16);
    int ok = take(move c);
    if ok != 1 { printf("pass move\n"); return 2; }

    printf("moves %d\n", moves);
    printf("move_return ok\n");
    return 0;
}
