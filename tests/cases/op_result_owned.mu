// op_result_owned — a member operator and a user `operator[]` return an owned value exactly like a
// free operator: initializing a local from it transfers that value instead of copying it again and
// leaking the temporary. `allocs == frees` at the end proves nothing was left behind.
unit op_result_owned;

extern int printf(const char* fmt, ...);

int allocs = 0;
int frees = 0;

struct Buf {
    int* p;
    Buf(int n) { p = alloc(8) as int*; allocs = allocs + 1; p[0] = n; }
    Buf(const ref Buf o) { p = alloc(8) as int*; allocs = allocs + 1; p[0] = o.p[0]; }
    Buf operator-() { Buf r = Buf(0 - p[0]); return move r; }
    Buf operator[](int i) { Buf r = Buf(p[0] + i); return move r; }
    ~Buf() { frees = frees + 1; if (p != null) { free(p); p = null; } }
}

int main() {
    {
        Buf a = Buf(1);
        Buf neg = -a;
        Buf sub = a[3];
        printf("values %d %d\n", neg.p[0], sub.p[0]);
    }
    printf("allocs %d frees %d\n", allocs, frees);
    return 0;
}
