// strings.mu — string len/s[i], char[N] copied from a string, puts(&buf[0])
unit strings;

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

// string parameter: iterate and print directly
void show(string s) {
    for i in 0..s.length() {
        putchar(s[i]);
    }
    putchar('\n');
}

int main() {
    // ---------- string is a builtin UTF-8 view; len(s) is the byte count ----------
    show("hello");                       // hello
    const string greeting = "Mulan";    // const binding
    show(greeting);                      // Mulan

    // ---------- s[i] yields a byte (char, unsigned); string literals may be indexed directly ----------
    int sum = 0;
    for i in 0..strlen("abc") {
        sum += "abc"[i];
    }
    print_int(sum); putchar('\n');       // 97 + 98 + 99 = 294

    // ---------- for-in iterates a string directly (value binding, char) ----------
    for ch in greeting {
        putchar(ch);
    }
    putchar('\n');                       // Mulan

    // ---------- char[N] initialized from a string: byte copy + NUL ----------
    char[20] buf = "Mulan";
    puts(&buf[0]);                       // Mulan

    // sizeof(buf) / sizeof(buf[0]) is the array capacity N, not the string length
    auto cap = sizeof(buf) / sizeof(buf[0]);
    if cap == 20 {
        puts("capacity is 20");
    }

    // ---------- Compute the string length by hand (find the NUL) ----------
    int n = 0;
    while buf[n] != 0 {
        n += 1;
    }
    print_int(n); putchar('\n');         // 5

    // ---------- Writing a NUL truncates the string ----------
    buf[2] = 0;
    puts(&buf[0]);                       // Mu

    // ---------- Full-capacity string: 7 bytes + NUL = 8 ----------
    char[8] c2 = "abcdefg";
    puts(&c2[0]);                        // abcdefg
    int c2n = 0;
    while c2[c2n] != 0 {
        c2n += 1;
    }
    print_int(c2n); putchar('\n');       // 7

    // ---------- char is just an unsigned byte and can be used as a number ----------
    auto code = 'A';
    print_int(code); putchar('\n');      // 65
    auto diff = 'a' - 'A';               // char promotes to int
    print_int(diff); putchar('\n');      // 32

    return 0;
}
