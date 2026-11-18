// Every value type is movable by default. A type that owns a raw pointer needs no `T(move T)` — the
// compiler moves it by copying the bits and emptying the source, which is then not destroyed; the
// resource changes owner exactly once.
unit move_default_movable;

extern int printf(const char* fmt, ...);

int live = 0;

struct Buf {
    int* p;
    int tag;
    Buf(int n) { p = alloc(4) as int*; p[0] = n; tag = n; live += 1; }
    ~Buf() {
        if p != null { free(p); live -= 1; }
    }
}

int main() {
    {
        Buf a = Buf(7);
        Buf b = move a;
        if b.tag != 7 { printf("tag %d\n", b.tag); return 1; }
        if b.p[0] != 7 { printf("payload %d\n", b.p[0]); return 2; }
        // The moved-from value cannot be read at all (a compile error), so the observable property is
        // that the resource has exactly one owner here and is freed exactly once below.
        if live != 1 { printf("live %d\n", live); return 4; }
    }
    if live != 0 { printf("freed %d\n", live); return 5; }
    printf("move_default_movable ok\n");
    return 0;
}
