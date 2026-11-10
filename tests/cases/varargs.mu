// varargs.mu — extern C 可变参数调用：`...`、C 默认实参提升、string → const char*
unit varargs;

extern int printf(const char* fmt, ...);

int main() {
    // ---- 基本类型 ----
    printf("%d %d\n", 42, -7);
    printf("%s\n", "hello");
    printf("%c%c\n", 'A', 'B');
    printf("%f %.2f\n", 1.5, 2.25);
    printf("%lld\n", 1234567890123);

    // ---- 默认实参提升：byte/short/bool → int，float → double ----
    byte b = 200;
    short s = 300 as short;
    float f = 0.5;
    bool t = true;
    printf("%d %d %d %.1f\n", b, s, t, f);

    // ---- 返回值：printf 返回写出的字符数 ----
    int n = printf("abc\n");
    printf("%d\n", n);

    // ---- 命名参数 + 多个可变参数 ----
    printf("%s=%d,%d\n", "pair", 1, 2);
    return 0;
}
