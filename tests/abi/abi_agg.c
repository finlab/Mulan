/* C side of tests/abi/abi_agg.mu.
 *
 * Implements the `extern` functions the Mu unit imports, and calls back into its `export`ed
 * functions. The Mu symbols are spelled with the compiler's mangling: `mu_<unit>_<function>`
 * followed by one `__<type code>` per parameter (`S_<unit>_<Type>` for a struct, `long` for a
 * primitive).
 */
#include <stdio.h>

struct Pair { long long a, b; };
struct Wide { long long a, b, c; };
struct Single { int v; };
struct Quad { int v[4]; };
struct Trio { unsigned char a, b, c; };
#pragma pack(push, 1)
struct PackedPair { int a, b; };
#pragma pack(pop)
union Bits { long long i; double d; };

/* ---- imported by the Mu unit ---- */

long long c_take_pair(struct Pair p) { return p.a * 10 + p.b; }

struct Pair c_make_pair(long long a, long long b) {
    struct Pair r;
    r.a = a;
    r.b = b;
    return r;
}

long long c_take_wide(struct Wide w) { return w.a * 100 + w.b * 10 + w.c; }

struct Wide c_make_wide(long long a, long long b, long long c) {
    struct Wide r;
    r.a = a;
    r.b = b;
    r.c = c;
    return r;
}

long long c_take_single(struct Single s) { return s.v + 1; }

struct Single c_make_single(int v) {
    struct Single r;
    r.v = v;
    return r;
}

long long c_take_trio(struct Trio t) { return t.a * 100 + t.b * 10 + t.c; }

long long c_take_packed_pair(struct PackedPair p) { return p.a * 100 + p.b; }

struct PackedPair c_make_packed_pair(int a, int b) {
    struct PackedPair r;
    r.a = a;
    r.b = b;
    return r;
}

long long c_take_quad(struct Quad q) {
    /* the parameter is a private copy: modifying it must not reach the caller */
    long long sum = q.v[0] + q.v[1] * 2 + q.v[2] * 3 + q.v[3] * 4;
    q.v[0] = 0;
    return sum;
}

long long c_take_bits(union Bits b) { return b.i + 1; }

/* ---- exported by the Mu unit ---- */

extern long long mu_abi_agg_mu_take_pair__S_abi_agg_Pair(struct Pair p);
extern struct Pair mu_abi_agg_mu_make_pair__long__long(long long a, long long b);
extern long long mu_abi_agg_mu_take_wide__S_abi_agg_Wide(struct Wide w);
extern struct Wide mu_abi_agg_mu_make_wide__long__long__long(long long a, long long b,
                                                            long long c);
extern long long mu_abi_agg_mu_take_quad__S_abi_agg_Quad(struct Quad q);

int run_checks(void) {
    struct Pair p;
    p.a = 3;
    p.b = 4;
    printf("mu pair %lld\n", mu_abi_agg_mu_take_pair__S_abi_agg_Pair(p));

    struct Pair q = mu_abi_agg_mu_make_pair__long__long(5, 6);
    printf("mu pair back %lld %lld\n", q.a, q.b);

    struct Wide w;
    w.a = 1;
    w.b = 2;
    w.c = 3;
    printf("mu wide %lld\n", mu_abi_agg_mu_take_wide__S_abi_agg_Wide(w));

    struct Wide w2 = mu_abi_agg_mu_make_wide__long__long__long(7, 8, 9);
    printf("mu wide back %lld %lld %lld\n", w2.a, w2.b, w2.c);

    struct Quad d;
    d.v[0] = 1;
    d.v[1] = 2;
    d.v[2] = 3;
    d.v[3] = 4;
    printf("mu quad %lld\n", mu_abi_agg_mu_take_quad__S_abi_agg_Quad(d));
    return 0;
}
