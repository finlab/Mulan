// unicode_escape.mu — `\u{...}` 转义：字符串按 UTF-8 编码，字符字面量限于单字节
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
    // ---------- ASCII 码点 ----------
    show("\u{41}\u{42}\u{43}");          // ABC
    print_int(len("\u{41}") as int); putchar('\n');   // 1

    // ---------- 2 / 3 / 4 字节码点按 UTF-8 编码 ----------
    show("\u{E9}");                      // é
    show("\u{4E2D}\u{6587}");            // 中文
    show("\u{1F600}");                   // 😀
    print_int(len("\u{E9}") as int); putchar('\n');     // 2
    print_int(len("\u{4E2D}") as int); putchar('\n');   // 3
    print_int(len("\u{1F600}") as int); putchar('\n');  // 4

    // ---------- UTF-8 字节 ----------
    show_bytes("\u{4E2D}");              // 228 184 173
    show_bytes("\u{1F600}");             // 240 159 152 128

    // ---------- 与源码里的原始 UTF-8 一致 ----------
    if "\u{4E2D}" == "中" {
        puts("same as raw utf-8");
    }

    // ---------- 字符字面量：单字节码点 ----------
    print_int('\u{41}'); putchar('\n');  // 65
    print_int('\u{E9}'); putchar('\n');  // 233（byte 按位模式）
    char c = '\u{FF}';                   // 位模式 0xFF → char 是 i8
    print_int(c); putchar('\n');         // -1

    // ---------- 大小写十六进制、前导零 ----------
    show("\u{0061}\u{6a}\u{0041}");      // ajA

    return 0;
}
