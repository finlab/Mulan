// bitwise.mu — & | ^ ~ (bitwise not), << >>, compound assignment &= |= ^= <<= >>=
unit bitwise;

extern int puts(const char* s);
extern int putchar(int c);

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

// print the low 8 bits of v in binary (m goes from 128 down to 1)
void print_bits(int v) {
    int m = 128;
    while m > 0 {
        if v & m != 0 {
            putchar('1');
        } else {
            putchar('0');
        }
        m >>= 1;
    }
}

int main() {
    int a = 12, b = 10;   // 1100, 1010

    print_int(a & b); putchar('\n');     //  8 (1000)
    print_int(a | b); putchar('\n');     // 14 (1110)
    print_int(a ^ b); putchar('\n');     //  6 (0110)

    // ~ is bitwise not: ~12 = -13 (32-bit two's complement)
    print_int(~a); putchar('\n');        // -13
    print_int(~0); putchar('\n');        // -1

    print_bits(a); putchar('\n');        // 00001100
    print_bits(a & b); putchar('\n');    // 00001000
    print_bits(a | b); putchar('\n');    // 00001110
    print_bits(a ^ b); putchar('\n');    // 00000110

    // shifts
    print_int(1 << 4); putchar('\n');    // 16
    print_int(256 >> 3); putchar('\n');  // 32
    print_int(-8 >> 1); putchar('\n');   // -4 (arithmetic right shift sign-extends)

    // compound assignment
    int x = 22;
    x &= 15; print_int(x); putchar('\n');  // 22&15 = 6
    x |= 24; print_int(x); putchar('\n');  // 6|24 = 30
    x ^= 30; print_int(x); putchar('\n');  // 30^30 = 0

    int y = 3;
    y <<= 3; print_int(y); putchar('\n');  // 24
    y >>= 1; print_int(y); putchar('\n');  // 12

    // set bit / clear bit / flip bit
    int flags = 0;
    flags |= 1 << 2;                       // set bit2 -> 4
    flags |= 1 << 5;                       // set bit5 -> 36
    print_int(flags); putchar('\n');       // 36
    flags &= ~(1 << 5);                    // clear bit5 -> 4
    print_int(flags); putchar('\n');       // 4
    flags ^= 1 << 2;                       // flip bit2 -> 0
    print_int(flags); putchar('\n');       // 0

    if flags == 0 and (36 >> 2) == 9 {
        puts("bitwise ok");
    }

    return 0;
}
