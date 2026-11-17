// const_array_bound.mu — a compile-time constant is a valid array length (SPEC 3.3.2/1694/2674): a
// unit-level `const`, an arithmetic form of it, a `comptime` builtin, and a `sizeof`-derived constant.
unit const_array_bound;

use std.comptime;

extern int puts(const char* s);

const int SIZE = 16;
const int WIDE = SIZE * 2;

struct P { int x; int y; }

const int N_P = sizeof(P) as int;

int[N_P] g;

int main() {
    int[SIZE] a = {};
    int[WIDE] b = {};
    int[std.comptime.ptr_bits / 16] c = {};
    g[0] = 1;

    if sizeof(a) != SIZE * (sizeof(int) as int) { puts("const bound failed"); return 1; }
    if sizeof(b) != WIDE * (sizeof(int) as int) { puts("arithmetic bound failed"); return 2; }
    if sizeof(c) != (std.comptime.ptr_bits / 16) * (sizeof(int) as int) {
        puts("comptime bound failed");
        return 3;
    }
    if sizeof(g) != N_P * (sizeof(int) as int) { puts("sizeof bound failed"); return 4; }
    puts("const array bound ok");
    return 0;
}
