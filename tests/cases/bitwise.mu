// bitwise.mu — & | ^ ~（按位取反）、<< >>、复合赋值 &= |= ^= <<= >>=
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

// 打印 v 的低 8 位二进制（按 m 从 128 降到 1）
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

    // ~ 是按位取反：~12 = -13（32 位补码）
    print_int(~a); putchar('\n');        // -13
    print_int(~0); putchar('\n');        // -1

    print_bits(a); putchar('\n');        // 00001100
    print_bits(a & b); putchar('\n');    // 00001000
    print_bits(a | b); putchar('\n');    // 00001110
    print_bits(a ^ b); putchar('\n');    // 00000110

    // 移位
    print_int(1 << 4); putchar('\n');    // 16
    print_int(256 >> 3); putchar('\n');  // 32
    print_int(-8 >> 1); putchar('\n');   // -4（补码右移符号扩展）

    // 复合赋值
    int x = 22;
    x &= 15; print_int(x); putchar('\n');  // 22&15 = 6
    x |= 24; print_int(x); putchar('\n');  // 6|24 = 30
    x ^= 30; print_int(x); putchar('\n');  // 30^30 = 0

    int y = 3;
    y <<= 3; print_int(y); putchar('\n');  // 24
    y >>= 1; print_int(y); putchar('\n');  // 12

    // 位置位 / 清零 / 翻转
    int flags = 0;
    flags |= 1 << 2;                       // 置位 bit2 -> 4
    flags |= 1 << 5;                       // 置位 bit5 -> 36
    print_int(flags); putchar('\n');       // 36
    flags &= ~(1 << 5);                    // 清 bit5 -> 4
    print_int(flags); putchar('\n');       // 4
    flags ^= 1 << 2;                       // 翻转 bit2 -> 0
    print_int(flags); putchar('\n');       // 0

    if flags == 0 and (36 >> 2) == 9 {
        puts("bitwise ok");
    }

    return 0;
}
