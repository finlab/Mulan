// varargs.mu — extern C variadic calls: `...`, C default argument promotions, string → const char*
unit varargs;

extern int printf(const char* fmt, ...);

int main() {
    // ---- basic types ----
    printf("%d %d\n", 42, -7);
    printf("%s\n", "hello");
    printf("%c%c\n", 'A', 'B');
    printf("%f %.2f\n", 1.5, 2.25);
    printf("%lld\n", 1234567890123);

    // ---- default arg promotions: byte/short/bool → int, float → double ----
    byte b = 200;
    short s = 300 as short;
    float f = 0.5;
    bool t = true;
    printf("%d %d %d %.1f\n", b, s, t, f);

    // ---- return value: printf returns the number of chars written ----
    int n = printf("abc\n");
    printf("%d\n", n);

    // ---- named arg + multiple variadic args ----
    printf("%s=%d,%d\n", "pair", 1, 2);
    return 0;
}
