// bool_ops.mu — precedence of not/and/or against comparisons
// Facts (decided by the parser.py grammar: not binds looser than == but tighter than and; and binds tighter than or; bitwise binds tighter than comparison):
//   not a == b  means  not (a == b); not a and b means (not a) and b; a or b and c means a or (b and c)
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
    // ---------- not a == b: not applies to the whole comparison (operands are int, the alternate parse does not even compile) ----------
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

    // ---------- not a and b: means (not a) and b, not not (a and b) ----------
    auto a = true;
    auto b = false;
    // a=true, b=false: (not true) and false = false; not (true and false) = true
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

    // ---------- and binds tighter than or: p or (q and r) ----------
    auto p = true, q = false, r = false;
    if p or q and r {
        puts("p or q and r is true");
    } else {
        puts("WRONG: (p or q) and r");
    }

    // ---------- == is left-associative: (1 < 2) == (2 < 3) ----------
    if 1 < 2 == 2 < 3 {
        puts("1 < 2 == 2 < 3 is true");
    } else {
        puts("1 < 2 == 2 < 3 is false");
    }

    // ---------- not applies to the whole result of != ----------
    if not 1 == 2 {
        puts("not 1 == 2 is true");
    }
    // not (1 != 2) = not true = false: nothing is printed

    // ---------- double negation returns the original value ----------
    if not not p {
        puts("not not p is true");
    }
    if not not q {
        puts("not not q is true");
    } else {
        puts("not not q is false");
    }

    // ---------- truth table of bool combinations ----------
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
