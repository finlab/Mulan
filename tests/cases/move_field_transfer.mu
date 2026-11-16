// move_field_transfer — a struct without a destructor is moved by the compiler: the bytes move and the
// source is emptied, so a reference-counted field changes owner exactly once
// (agents/value-semantics.md §8.2).
unit move_field_transfer;

extern int printf(const char* fmt, ...);

int live = 0;

class Cat {
    public int v;
    Cat() { live = live + 1; }
    ~Cat() { live = live - 1; }
}

struct Holder { Cat c; int n; }

int main() {
    {
        Holder a;
        a.c = new Cat();
        a.n = 7;
        if live != 1 { printf("ctor live %d\n", live); return 1; }

        Holder b = move a;      // implicit transfer: no move constructor needed
        if live != 1 { printf("transfer live %d\n", live); return 2; }
        if b.c == null { printf("destination\n"); return 3; }
        if b.n != 7 { printf("payload %d\n", b.n); return 4; }
    }
    if live != 0 { printf("release live %d\n", live); return 5; }

    printf("move_field_transfer ok\n");
    return 0;
}
