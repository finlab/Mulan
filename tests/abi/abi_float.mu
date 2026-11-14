// The C ABI of float-bearing aggregates (SPEC 7.2): on Win64 an aggregate whose leaves are all
// float/double travels in an SSE register, so an 8-byte `{float, float}` must arrive as one packed
// XMM value rather than in two. A mixed `{float, int}` is integer-classified.
unit abi_float;

use std.cstdio;

// 8 bytes, two floats: one packed XMM register.
public struct FF {
    public float x;
    public float y;
}

// 4 bytes, one float: a single XMM register (its own LLVM type already matches).
public struct F1 {
    public float x;
}

// 8 bytes, one double.
public struct D1 {
    public double d;
}

// 8 bytes, mixed: integer-classified, so the bits travel in a general register.
public struct FL {
    public float f;
    public int i;
}

// ---------------------------------------------------------------- implemented in abi_float.c
extern FF c_ff(FF v);
extern F1 c_f1(F1 v);
extern D1 c_d1(D1 v);
extern FL c_fl(FL v);
extern int c_probe_exports();

// ---------------------------------------------------------------- called from abi_float.c
export FF mu_ff(FF v) {
    FF r = { x = v.x * 2.0f, y = v.y * 3.0f };
    return r;
}

export F1 mu_f1(F1 v) {
    F1 r = { x = v.x * 10.0f };
    return r;
}

export D1 mu_d1(D1 v) {
    D1 r = { d = v.d * 4.0 };
    return r;
}

export FL mu_fl(FL v) {
    FL r = { f = v.f + 0.5f, i = v.i + 1 };
    return r;
}

int main() {
    int score = 0;

    FF a = { x = 1.5f, y = 2.5f };
    FF b = c_ff(a);
    if b.x == 2.5f { score = score + 1; }
    if b.y == 4.5f { score = score + 1; }

    F1 c1 = { x = 1.25f };
    F1 c = c_f1(c1);
    if c.x == 12.5f { score = score + 1; }

    D1 d1 = { d = 1.5 };
    D1 d = c_d1(d1);
    if d.d == 6.0 { score = score + 1; }

    FL e1 = { f = 1.5f, i = 10 };
    FL e = c_fl(e1);
    if e.f == 2.0f { score = score + 1; }
    if e.i == 11 { score = score + 1; }

    // the same shapes through the exported functions, called from Mu
    FF f = mu_ff(a);
    if f.x == 3.0f { score = score + 1; }
    if f.y == 7.5f { score = score + 1; }
    F1 g1 = { x = 2.0f };
    F1 g = mu_f1(g1);
    if g.x == 20.0f { score = score + 1; }
    D1 h1 = { d = 1.25 };
    D1 h = mu_d1(h1);
    if h.d == 5.0 { score = score + 1; }
    FL k1 = { f = 1.0f, i = 41 };
    FL k = mu_fl(k1);
    if k.f == 1.5f { score = score + 1; }
    if k.i == 42 { score = score + 1; }

    printf("float abi score %d\n", score);
    printf("float abi from c %d\n", c_probe_exports());
    return 0;
}
