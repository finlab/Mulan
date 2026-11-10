// enum_cmp.mu — 枚举只与同一枚举比较（== != < <= > >=），与整数双向显式 `as`
unit enum_cmp;

extern int puts(const char* s);
extern int putchar(int c);

enum Level { Low, Mid = 5, High }

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
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
    Level a = Level.Low;
    Level b = Level.Mid;
    Level c = Level.High;

    if a == Low and a != b { puts("eq ok"); }
    if Level.Low < b and a <= Level.Mid and b <= b { puts("lt ok"); }
    if c > b and c >= b and b >= b { puts("gt ok"); }
    if not (c < b) and not (a > b) { puts("not ok"); }

    // 序 = 底层整数值（Mid = 5），不是声明顺序
    int bi = b as int;
    print_int(bi);
    putchar('\n');
    Level back = 6 as Level;
    if back == Level.High { puts("roundtrip ok"); }

    // 显式转出后可以算术
    int hi = (c as int) + 1;
    print_int(hi);
    putchar('\n');

    return 0;
}
