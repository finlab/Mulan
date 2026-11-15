// The C ABI of aggregates, part 2 (SPEC 7.2): the shapes that pin down argument *ordering* and
// nesting — an 8-byte struct (register path), a struct whose field is a struct, and a call that
// mixes a struct result (hidden pointer) with a struct parameter and scalars.
unit abi_mix;

use std.cstdio;

// 8 bytes: 1/2/4/8 so it travels in a register, like a scalar.
public struct Two {
    public int a;
    public int b;
}

// 16 bytes with a nested struct: by reference.
public struct Box {
    public Two t;
    public long pad;
}

// 16 bytes, the plainest by-reference/by-return case.
public struct Pair {
    public long a;
    public long b;
}

// 24 bytes.
public struct Wide {
    public long a;
    public long b;
    public long c;
}

// 12 bytes: two INTEGER eightbytes, the second one only half filled — the shape that shows whether
// the register pair is stored into a wide-enough slot.
public struct Three {
    public int a;
    public int b;
    public int c;
}

// 6 bytes / align 2: one INTEGER register, and a size that is not a power of two (the slot's
// alignment must come from the fields, never from `size`).
public struct Six {
    public short a;
    public short b;
    public short c;
}

// ---------------------------------------------------------------- implemented in abi_mix.c
extern Two c_eight(Two t);
extern Box c_wrap(long v);
extern Pair c_mix(Pair p, long k, Wide w);
extern Three c_three(Three t);
extern Six c_six(Six s);
extern void c_call_exports(long* out);

// ---------------------------------------------------------------- called from abi_mix.c
export Two mu_eight(Two t) {
    Two r = { a = t.a * 2, b = t.b * 3 };
    return r;
}

export Box mu_wrap(long v) {
    Box b = { t = { a = v as int, b = (v + 1) as int }, pad = v * 100 };
    return b;
}

export Pair mu_mix(Pair p, long k, Wide w) {
    Pair r = { a = p.a + k + w.a, b = p.b + w.c };
    return r;
}

export Three mu_three(Three t) {
    Three r = { a = t.a * 2, b = t.b * 3, c = t.c * 4 };
    return r;
}

export Six mu_six(Six s) {
    Six r = { a = (s.a + 1) as short, b = (s.b + 1) as short, c = (s.c + 1) as short };
    return r;
}

int main() {
    Two e = { a = 10, b = 20 };
    Two e2 = c_eight(e);
    printf("c eight %d %d\n", e2.a, e2.b);

    Box bx = c_wrap(5);
    printf("c wrap %d %d %d\n", bx.t.a, bx.t.b, bx.pad as int);

    Pair p = { a = 1, b = 2 };
    Wide w = { a = 3, b = 4, c = 5 };
    Pair m = c_mix(p, 10, w);
    printf("c mix %d %d\n", m.a as int, m.b as int);

    // 12 and 6 bytes: the slot for the two-register form must hold both halves.
    Three th = { a = 1, b = 2, c = 3 };
    Three th2 = c_three(th);
    printf("c three %d %d %d\n", th2.a, th2.b, th2.c);
    Six sx = { a = 1, b = 2, c = 3 };
    Six sx2 = c_six(sx);
    printf("c six %d %d %d\n", sx2.a as int, sx2.b as int, sx2.c as int);

    // the same three through the exported functions, called from Mu (one symbol, one ABI)
    Two d2 = mu_eight(e);
    printf("direct eight %d %d\n", d2.a, d2.b);
    Box db = mu_wrap(4);
    printf("direct wrap %d %d %d\n", db.t.a, db.t.b, db.pad as int);
    Pair dm = mu_mix(p, 100, w);
    printf("direct mix %d %d\n", dm.a as int, dm.b as int);

    // and once more with the calls made from C
    long[5] out = {};
    c_call_exports(&out[0]);
    printf("c calls %d %d %d %d %d\n", out[0] as int, out[1] as int, out[2] as int,
           out[3] as int, out[4] as int);
    return 0;
}
