// string_library.mu — `string` is a pure library type (0.0.89): a literal is `const char*`,
// `implicit string(const char*)` builds a string, and every operation is library code
unit string_library;

extern int putchar(int c);
extern int puts(const char* s);

void print_int(int v) {
    if v == 0 { putchar('0'); return; }
    if v < 0 { putchar('-'); v = -v; }
    int[20] digits;
    int i = 0;
    while v > 0 { digits[i] = v % 10; v /= 10; i += 1; }
    while i > 0 { i -= 1; putchar('0' + digits[i]); }
}

int main() {
    // a literal is a `const char*`: no allocation, direct C interop
    const char* raw = "utf8 \u{4E2D}";
    puts(raw);
    print_int(strlen(raw) as int); putchar('\n');          // 8 (5 ASCII bytes + 3 UTF-8 bytes)
    puts("literal direct");

    // implicit construction: one allocation + copy
    string s = "hello";
    print_int(s.length() as int); putchar('\n');            // 5
    print_int(s[1] as int); putchar('\n');                  // 101

    // byte iteration through the index form (length() + member operator[])
    int n = 0;
    for c in s { n += 1; }
    print_int(n); putchar('\n');                            // 5

    // comparison against literals, both directions
    if (s == "hello") { putchar('E'); }
    if (s != "world") { putchar('N'); }
    if ("hello" == s) { putchar('M'); }
    putchar('\n');                                          // ENM

    // concatenation (member operator+)
    string t = s + "!";
    if (t == "hello!") { puts("concat ok"); }
    string u = "!" + s;
    if (u == "!hello") { puts("left concat ok"); }

    // C interop: cstr() + char[N] initialized from a literal
    puts(t.cstr());
    char[8] buf = "hi";
    if buf[0] == 'h' and buf[1] == 'i' and buf[2] == 0 { puts("char array ok"); }

    // zero value
    string z = {};
    if z.length() == 0 and z == "" { puts("zero ok"); }
    return 0;
}
