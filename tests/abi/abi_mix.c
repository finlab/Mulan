/* C side of tests/abi/abi_mix.mu: nested and mixed-argument shapes.
 * Mu symbols are `mu_<unit>_<function>` plus one `__<type code>` per parameter. */
#include <stdio.h>

struct Two { int a, b; };
struct Box { struct Two t; long long pad; };
struct Pair { long long a, b; };
struct Wide { long long a, b, c; };
struct Three { int a, b, c; };
struct Six { short a, b, c; };

/* ---- imported by the Mu unit ---- */

struct Two c_eight(struct Two t) {
    struct Two r;
    r.a = t.a + 1;
    r.b = t.b + 2;
    return r;
}

struct Box c_wrap(long long v) {
    struct Box b;
    b.t.a = (int)v;
    b.t.b = (int)(v + 1);
    b.pad = v * 10;
    return b;
}

/* hidden result pointer + one by-reference struct + a scalar + another by-reference struct */
struct Pair c_mix(struct Pair p, long long k, struct Wide w) {
    struct Pair r;
    r.a = p.a + k + w.a;
    r.b = p.b + w.c;
    return r;
}

/* 12 bytes: two INTEGER eightbytes on SysV, a hidden pointer on MS x64; either way both halves of the
 * third int must survive the round trip. */
struct Three c_three(struct Three t) {
    t.a += 1;
    t.b += 2;
    t.c += 3;
    return t;
}

/* 6 bytes, align 2: one INTEGER register on SysV; the size is not a power of two. */
struct Six c_six(struct Six s) {
    s.a += 1;
    s.b += 2;
    s.c += 3;
    return s;
}

/* ---- exported by the Mu unit ---- */

extern struct Two mu_abi_mix_mu_eight__S_abi_mix_Two(struct Two t);
extern struct Box mu_abi_mix_mu_wrap__long(long long v);
extern struct Pair mu_abi_mix_mu_mix__S_abi_mix_Pair__long__S_abi_mix_Wide(struct Pair p,
                                                                           long long k,
                                                                           struct Wide w);
extern struct Three mu_abi_mix_mu_three__S_abi_mix_Three(struct Three t);
extern struct Six mu_abi_mix_mu_six__S_abi_mix_Six(struct Six s);

/* Call every export once from C, so the C-compiled call sites are checked too. The results are
 * returned through `out` and printed by the Mu side. */
void c_call_exports(long long* out) {
    struct Two t;
    t.a = 1;
    t.b = 2;
    struct Two r = mu_abi_mix_mu_eight__S_abi_mix_Two(t);
    out[0] = r.a * 10 + r.b;

    struct Box b = mu_abi_mix_mu_wrap__long(3);
    out[1] = b.t.a * 100 + b.t.b * 10 + b.pad;

    struct Pair p;
    p.a = 1;
    p.b = 2;
    struct Wide w;
    w.a = 3;
    w.b = 4;
    w.c = 5;
    struct Pair m = mu_abi_mix_mu_mix__S_abi_mix_Pair__long__S_abi_mix_Wide(p, 10, w);
    out[2] = m.a * 10 + m.b;

    struct Three th;
    th.a = 1;
    th.b = 2;
    th.c = 3;
    struct Three th2 = mu_abi_mix_mu_three__S_abi_mix_Three(th);
    out[3] = th2.a * 100 + th2.b * 10 + th2.c;

    struct Six sx;
    sx.a = 1;
    sx.b = 2;
    sx.c = 3;
    struct Six sx2 = mu_abi_mix_mu_six__S_abi_mix_Six(sx);
    out[4] = sx2.a * 100 + sx2.b * 10 + sx2.c;
}
