// copy_param_handover — a by-value struct argument is copied through the user copy constructor and
// handed to the callee, which releases it (SPEC 3.4 rule of three, 3.8 owning parameters). The
// caller's object stays intact; before the fix the shallow copy was released by the callee and the
// caller's pointer dangled (a silent double free at scope exit).
unit copy_param_handover;

extern int printf(const char* fmt, ...);

int frees = 0;

struct Buf {
    int* p;
    Buf(int n) { p = alloc(8) as int*; p[0] = n; }
    Buf(const ref Buf o) { p = alloc(8) as int*; p[0] = o.p[0]; }
    ~Buf() { frees = frees + 1; if (p != null) { free(p); p = null; } }
}

void take(Buf b) { }

int main() {
    Buf a = Buf(7);
    take(a);
    printf("after first %d %d\n", a.p[0], frees);
    take(a);
    printf("after second %d %d\n", a.p[0], frees);
    return 0;
}
