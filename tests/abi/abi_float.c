/* C side of tests/abi/abi_float.mu: float-only and mixed aggregates (SSE vs integer class). */
#include <stdio.h>

struct FF { float x, y; };
struct F1 { float x; };
struct D1 { double d; };
struct FL { float f; int i; };

/* ---- imported by the Mu unit ---- */

struct FF c_ff(struct FF v) {
    struct FF r;
    r.x = v.x + 1.0f;
    r.y = v.y + 2.0f;
    return r;
}

struct F1 c_f1(struct F1 v) {
    struct F1 r;
    r.x = v.x * 10.0f;
    return r;
}

struct D1 c_d1(struct D1 v) {
    struct D1 r;
    r.d = v.d * 4.0;
    return r;
}

struct FL c_fl(struct FL v) {
    struct FL r;
    r.f = v.f + 0.5f;
    r.i = v.i + 1;
    return r;
}

/* ---- exported by the Mu unit ---- */

extern struct FF mu$abi_float$mu__ff$S$abi_float$FF(struct FF v);
extern struct F1 mu$abi_float$mu__f1$S$abi_float$F1(struct F1 v);
extern struct D1 mu$abi_float$mu__d1$S$abi_float$D1(struct D1 v);
extern struct FL mu$abi_float$mu__fl$S$abi_float$FL(struct FL v);

/* Call every export once from C and return how many results are exactly right. */
int c_probe_exports(void) {
    int score = 0;
    struct FF a;
    a.x = 1.5f;
    a.y = 2.5f;
    struct FF b = mu$abi_float$mu__ff$S$abi_float$FF(a);
    if (b.x == 3.0f) score++;
    if (b.y == 7.5f) score++;

    struct F1 c;
    c.x = 2.0f;
    struct F1 d = mu$abi_float$mu__f1$S$abi_float$F1(c);
    if (d.x == 20.0f) score++;

    struct D1 e;
    e.d = 1.25;
    struct D1 f = mu$abi_float$mu__d1$S$abi_float$D1(e);
    if (f.d == 5.0) score++;

    struct FL g;
    g.f = 1.0f;
    g.i = 41;
    struct FL h = mu$abi_float$mu__fl$S$abi_float$FL(g);
    if (h.f == 1.5f) score++;
    if (h.i == 42) score++;
    return score;
}
