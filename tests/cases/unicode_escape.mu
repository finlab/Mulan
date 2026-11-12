// unicode_escape.mu — `\u{...}` escapes: strings encoded as UTF-8, char literals limited to one byte
unit unicode_escape;

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

void show(string s) {
    for i in 0..len(s) {
        putchar(s[i]);
    }
    putchar('\n');
}

void show_bytes(string s) {
    for i in 0..len(s) {
        print_int(s[i]);
        putchar(' ');
    }
    putchar('\n');
}

int main() {
    // ---------- ASCII code points ----------
    show("\u{41}\u{42}\u{43}");          // ABC
    print_int(len("\u{41}") as int); putchar('\n');   // 1

    // ---------- 2 / 3 / 4 byte code points encoded as UTF-8 ----------
    show("\u{E9}");                      // é
    show("\u{4E2D}\u{6587}");            // Chinese text
    show("\u{1F600}");                   // 😀
    print_int(len("\u{E9}") as int); putchar('\n');     // 2
    print_int(len("\u{4E2D}") as int); putchar('\n');   // 3
    print_int(len("\u{1F600}") as int); putchar('\n');  // 4

    // ---------- UTF-8 bytes ----------
    show_bytes("\u{4E2D}");              // 228 184 173
    show_bytes("\u{1F600}");             // 240 159 152 128

    // ---------- same as raw UTF-8 in the source ----------
    if "\u{4E2D}" == "中" {
        puts("same as raw utf-8");
    }

    // ---------- char literals: single-byte code points ----------
    print_int('\u{41}'); putchar('\n');  // 65
    print_int('\u{E9}'); putchar('\n');  // 233 (byte as bit pattern)
    char c = '\u{FF}';                   // bit pattern 0xFF → char is i8
    print_int(c); putchar('\n');         // -1

    // ---------- upper/lowercase hex, leading zeros ----------
    show("\u{0061}\u{6a}\u{0041}");      // ajA

    return 0;
}
