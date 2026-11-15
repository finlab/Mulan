// unsigned_wrap.mu — unsigned wraparound, division and modulo
//
// Unsigned arithmetic is defined to wrap (two's complement, SPEC 8.2); this case
// locks the wrap, the unsigned division/modulo and the int -> uint reinterpretation.
unit unsigned_wrap;

use std.cstdio;

int main() {
    // ---- uint wraparound ----
    uint u = 0xFFFFFFFF;
    u += 1;                         // wraps to 0
    printf("u=%u\n", u);

    uint w = 4;
    w = w - 5;                      // wraps to 4294967295
    printf("w=%u\n", w);

    // ---- ulong wraparound ----
    // 9223372036854775807 is the largest literal (2^63-1); hex and decimal literals
    // above that are rejected, so the wrap is reached by arithmetic.
    ulong v = 9223372036854775807;
    v = v * 2 + 1;                  // 2^64-1
    printf("v=%llu\n", v);
    v += 1;                         // wraps to 0
    printf("v=%llu\n", v);

    // ---- division and modulo are unsigned ----
    uint big = 4294967295;
    printf("div=%u mod=%u\n", big / 3, big % 7);

    // ---- an int cast to uint is reinterpreted, not sign extended ----
    int b = -1;
    int a = 3;
    printf("cast=%d\n", (b as uint) > a);

    // ---- `>>` follows the operand's signedness: 0x80000000 >> 4 is 0x08000000 (logical),
    //      while a signed value sign-extends (SPEC 5) ----
    uint sh = 0x80000000;
    printf("shr=%u\n", sh >> 4);

    puts("unsigned wrap ok");
    return 0;
}
