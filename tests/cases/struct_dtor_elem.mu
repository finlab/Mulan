// struct_dtor_elem — a struct with a user destructor is a legal dynamic array element: `new S[n]` runs the
// default constructor of every element and releasing the block destroys them
// (agents/value-semantics.md §7).
unit struct_dtor_elem;

extern int printf(const char* fmt, ...);

int live = 0;
int dtors = 0;

struct S {
    int x;
    S() { x = 7; live = live + 1; }
    ~S() { dtors = dtors + 1; live = live - 1; }
}

int main() {
    {
        S[] a = new S[3];
        if a.length() != 3 { printf("length\n"); return 1; }
        if a[0].x != 7 or a[2].x != 7 { printf("element not constructed\n"); return 2; }
        if live != 3 { printf("live %d\n", live); return 3; }
    }
    if live != 0 { printf("leaked %d\n", live); return 4; }
    if dtors != 3 { printf("dtors %d\n", dtors); return 5; }
    printf("struct_dtor_elem ok\n");
    return 0;
}
