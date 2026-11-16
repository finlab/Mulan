// move_ctor — explicit move construction (agents/value-semantics.md §8.3): `move x` transfers ownership,
// the source becomes an empty shell that is no longer usable, and
// `T(move T other)` is what makes an uncopyable type movable.
unit move_ctor;

extern int printf(const char* fmt, ...);

int moves = 0;
int live = 0;

nocopy struct Buf {
    int* p;
    Buf(int n) { p = alloc(n) as int*; }
    ~Buf() { free(p); }
    Buf(move Buf o) {
        p = o.p;
        moves = moves + 1;
        live = live + 1;
    }
}

int main() {
    {
        Buf a = Buf(16);
        if a.p == null { printf("alloc\n"); return 1; }

        Buf b = move a;              // move construction: `a` is out of reach from here on
        if b.p == null { printf("destination empty\n"); return 2; }
        if moves != 1 { printf("move count %d\n", moves); return 3; }
    }
    if live != 1 { printf("live %d\n", live); return 4; }

    printf("moves %d live %d\n", moves, live);
    printf("move_ctor ok\n");
    return 0;
}
