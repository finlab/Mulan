// strings.mu — string 的 len/s[i]、char[N] 从 string 拷贝、puts(&buf[0])
unit strings;

extern int puts(readonly char* s);
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

// string 参数：直接遍历打印
void show(string s) {
    for i in 0..len(s) {
        putchar(s[i]);
    }
    putchar('\n');
}

int main() {
    // ---------- string 是内建 UTF-8 视图，len(s) 是字节数 ----------
    show("hello");                       // hello
    const string greeting = "mu-lan";    // const 绑定
    show(greeting);                      // mu-lan

    // ---------- s[i] 取字节（char，无符号）；可直接下标字符串字面量 ----------
    int sum = 0;
    for i in 0..len("abc") {
        sum += "abc"[i];
    }
    print_int(sum); putchar('\n');       // 97 + 98 + 99 = 294

    // ---------- for-in 直接遍历 string（值绑定，char） ----------
    for ch in greeting {
        putchar(ch);
    }
    putchar('\n');                       // mu-lan

    // ---------- char[N] 从 string 初始化：字节拷贝 + NUL ----------
    char[20] buf = "mu-lan";
    puts(&buf[0]);                       // mu-lan

    // len(buf) 是数组容量 N，不是字符串长度
    auto cap = len(buf);
    if cap == 20 {
        puts("capacity is 20");
    }

    // ---------- 手动求字符串长度（找 NUL） ----------
    int n = 0;
    while buf[n] != 0 {
        n += 1;
    }
    print_int(n); putchar('\n');         // 6

    // ---------- 写入 NUL 截断字符串 ----------
    buf[2] = 0;
    puts(&buf[0]);                       // mu

    // ---------- 满容量字符串：7 字节 + NUL = 8 ----------
    char[8] c2 = "abcdefg";
    puts(&c2[0]);                        // abcdefg
    int c2n = 0;
    while c2[c2n] != 0 {
        c2n += 1;
    }
    print_int(c2n); putchar('\n');       // 7

    // ---------- char 就是无符号字节，可当数值用 ----------
    auto code = 'A';
    print_int(code); putchar('\n');      // 65
    auto diff = 'a' - 'A';               // char 提升为 int
    print_int(diff); putchar('\n');      // 32

    return 0;
}
