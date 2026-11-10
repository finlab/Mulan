// bool_ops.mu — not/and/or 与比较的优先级
// 事实（由 parser.py 的文法决定，not 比 == 松、比 and 紧；and 比 or 紧；位运算比比较紧）：
//   not a == b  即  not (a == b)；not a and b 即 (not a) and b；a or b and c 即 a or (b and c)
unit bool_ops;

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
    // ---------- not a == b：not 作用于整个比较（操作数是 int，alternate parse 根本无法编译） ----------
    int x = 5, y = 5;
    if not x == y {
        puts("WRONG: not x == y as ((not x) == y)");
    } else {
        puts("not x == y means not (x == y)");
    }
    int a2 = 3, b2 = 7;
    if not a2 == b2 {
        puts("not 3 == 7 is true");
    }

    // ---------- not a and b：等价 (not a) and b，而非 not (a and b) ----------
    auto a = true;
    auto b = false;
    // a=true, b=false: (not true) and false = false；not (true and false) = true
    if not a and b {
        puts("not a and b is true");
    } else {
        puts("not a and b is false");
    }
    if not (a and b) {
        puts("not (a and b) is true");
    } else {
        puts("not (a and b) is false");
    }

    // ---------- and 比 or 紧：p or (q and r) ----------
    auto p = true, q = false, r = false;
    if p or q and r {
        puts("p or q and r is true");
    } else {
        puts("WRONG: (p or q) and r");
    }

    // ---------- == 左结合：(1 < 2) == (2 < 3) ----------
    if 1 < 2 == 2 < 3 {
        puts("1 < 2 == 2 < 3 is true");
    } else {
        puts("1 < 2 == 2 < 3 is false");
    }

    // ---------- not 作用于 != 的整个结果 ----------
    if not 1 == 2 {
        puts("not 1 == 2 is true");
    }
    // not (1 != 2) = not true = false：不打印

    // ---------- 双重否定回到原值 ----------
    if not not p {
        puts("not not p is true");
    }
    if not not q {
        puts("not not q is true");
    } else {
        puts("not not q is false");
    }

    // ---------- bool 组合真值表 ----------
    for i in 0..4 {
        auto s = i >= 2;         // false false true true
        auto t = i % 2 == 0;     // true  false true false
        print_int(i); putchar(' ');
        if s and t { putchar('A'); }
        if s and not t { putchar('B'); }
        if not s and t { putchar('C'); }
        if not s and not t { putchar('D'); }
        putchar('\n');
    }

    return 0;
}
