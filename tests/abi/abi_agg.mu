// The C ABI of aggregates (SPEC 7.2): structs and unions passed and returned *by value* across the
// boundary. `extern` calls into the C shim, and the shim calls back into the `export`ed functions,
// so a wrong convention shows up as wrong numbers (or a crash) rather than passing silently.
unit abi_agg;

use std.cstdio;

// 16 bytes: on Win64 not 1/2/4/8 bytes, so C passes it by reference (`byval`) and returns it
// through a hidden pointer (`sret`).
public struct Pair {
    public long a;
    public long b;
}

// 24 bytes: by reference on both ABIs.
public struct Wide {
    public long a;
    public long b;
    public long c;
}

// 4 bytes: the register path (unchanged by the C ABI).
public struct Single {
    public int v;
}

// 16 bytes with an array field: still by reference, the array stays inside.
public struct Quad {
    public int[4] v;
}

// 3 bytes, alignment 1: by reference on Win64 (not 1/2/4/8 bytes).
packed struct Trio {
    byte a;
    byte b;
    byte c;
}

// 8 bytes but alignment 1: still a register-sized aggregate (the "integer of the same size" rule),
// with the bits reinterpreted through a stack slot that must satisfy the i64 access alignment.
packed struct PackedPair {
    int a;
    int b;
}

// A union travels as its size: 8 bytes here, so the register path.
union Bits {
    long i;
    double d;
}

// ---------------------------------------------------------------- implemented in abi_agg.c
extern long c_take_pair(Pair p);
extern Pair c_make_pair(long a, long b);
extern long c_take_wide(Wide w);
extern Wide c_make_wide(long a, long b, long c);
extern long c_take_single(Single s);
extern Single c_make_single(int v);
extern long c_take_trio(Trio t);
extern long c_take_packed_pair(PackedPair p);
extern PackedPair c_make_packed_pair(int a, int b);
extern long c_take_quad(Quad q);
extern long c_take_bits(Bits b);
extern int run_checks();

// ---------------------------------------------------------------- called from abi_agg.c
export long mu_take_pair(Pair p) { return p.a * 10 + p.b; }

export Pair mu_make_pair(long a, long b) {
    Pair r = { a = a, b = b };
    return r;
}

export long mu_take_wide(Wide w) { return w.a * 100 + w.b * 10 + w.c; }

export Wide mu_make_wide(long a, long b, long c) {
    Wide r = { a = a, b = b, c = c };
    return r;
}

export long mu_take_quad(Quad q) { return q.v[0] + q.v[1] * 2 + q.v[2] * 3 + q.v[3] * 4; }

int main() {
    // Mu -> C
    Pair p = { a = 3, b = 4 };
    printf("c pair %d\n", c_take_pair(p) as int);
    Pair q = c_make_pair(5, 6);
    printf("c pair back %d %d\n", q.a as int, q.b as int);

    Wide w = { a = 1, b = 2, c = 3 };
    printf("c wide %d\n", c_take_wide(w) as int);
    Wide w2 = c_make_wide(7, 8, 9);
    printf("c wide back %d %d %d\n", w2.a as int, w2.b as int, w2.c as int);

    Single s = { v = 11 };
    printf("c single %d\n", c_take_single(s) as int);
    Single s2 = c_make_single(12);
    printf("c single back %d\n", s2.v);

    Trio t = { a = 1, b = 2, c = 3 };
    printf("c trio %d\n", c_take_trio(t) as int);

    PackedPair pp = { a = 11, b = 22 };
    printf("c packed pair %d\n", c_take_packed_pair(pp) as int);
    PackedPair pp2 = c_make_packed_pair(7, 8);
    printf("c packed pair back %d %d\n", pp2.a, pp2.b);

    Quad d = {};
    d.v[0] = 1;
    d.v[1] = 2;
    d.v[2] = 3;
    d.v[3] = 4;
    printf("c quad %d\n", c_take_quad(d) as int);

    Bits bits;
    bits.i = 41;
    printf("c bits %d\n", c_take_bits(bits) as int);

    // C -> Mu (the shim calls the exported functions and prints their results itself)
    printf("checks %d\n", run_checks());

    // Mu -> export -> Mu: a call site of an exported function must use the same C ABI the symbol
    // was emitted with (one symbol, one convention), so this goes through byval/sret too.
    Pair r = mu_make_pair(9, 8);
    printf("direct pair back %d %d\n", r.a as int, r.b as int);
    printf("direct take %d\n", mu_take_wide(w) as int);
    return 0;
}
