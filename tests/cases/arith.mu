// arith.mu — arithmetic, modulo, shifts, precedence (bitwise vs comparison), while accumulation
unit arith;

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

int main() {
    // ---------- Arithmetic and modulo (/ and % truncate toward zero) ----------
    print_int(7 + 3 * 2); putchar('\n');     // 13: * binds tighter than +
    print_int((7 + 3) * 2); putchar('\n');   // 20: parentheses first
    print_int(20 / 6); putchar('\n');        // 3: integer division truncates
    print_int(20 % 6); putchar('\n');        // 2
    print_int(-(3 + 4) * 2); putchar('\n');  // -14: unary minus binds to (3+4)
    print_int(100 - 7 - 8); putchar('\n');   // 85: - is left-associative

    // ---------- Shifts and +: + binds tighter than << and >> (C precedence convention) ----------
    print_int(1 << 2 + 1); putchar('\n');    // 1 << 3 = 8
    print_int((1 << 2) + 1); putchar('\n');  // 5
    print_int(16 >> 2 + 1); putchar('\n');   // 16 >> 3 = 2
    print_int((16 >> 2) + 1); putchar('\n'); // 5

    // ---------- & and +: + binds tighter (addition first) ----------
    print_int(9 & 3 + 4); putchar('\n');     // 9 & 7 = 1
    print_int((9 & 3) + 4); putchar('\n');   // 1 + 4 = 5

    // ---------- ^ and &: & binds tighter ----------
    print_int(12 & 5 ^ 3); putchar('\n');    // (12&5)=4, 4^3 = 7
    print_int(12 & (5 ^ 3)); putchar('\n');  // 12&6 = 4

    // ---------- | and &: | binds looser ----------
    print_int(3 & 12 | 10); putchar('\n');   // (3&12)|10 = 0|10 = 10
    print_int(3 & (12 | 10)); putchar('\n'); // 3&14 = 2

    // ---------- Bitwise binds tighter than comparison: 6 & 2 == 2 means (6 & 2) == 2 ----------
    if 6 & 2 == 2 {
        puts("bitwise binds tighter than ==");
    }
    // 4 < 6 & 2 parses as 4 < (6 & 2) = 4 < 2 = false
    if 4 < 6 & 2 {
        puts("WRONG: relational bound tighter");
    } else {
        puts("relational binds looser than |");
    }

    // ---------- while accumulation ----------
    int total = 0;
    int k = 1;
    while k <= 10 {
        total += k;
        k += 1;
    }
    print_int(total); putchar('\n');         // 55

    // ---------- Other compound assignments ----------
    int acc = 12;
    acc *= 5;                                // 60
    acc /= 4;                                // 15
    acc -= 1;                                // 14
    print_int(acc); putchar('\n');           // 14

    return 0;
}
