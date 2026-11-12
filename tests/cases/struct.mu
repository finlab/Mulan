// struct.mu — struct value semantics and the various initialization forms
// Note: `struct` is a keyword, so the unit name is struct_case (the file name stays struct.mu).
unit struct_case;

extern int puts(const char* s);
extern int putchar(int c);

struct Point {
    int x, y;
}

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

// Value parameter: modifications inside the function hit a copy
void nudge(Point p) {
    p.x += 100;
}

Point make_point(int x, int y) {
    auto p = Point();    // no-arg construction: all zeros
    p.x = x;
    p.y = y;
    return p;
}

int main() {
    // ---------- Positional init {3, 4} and value-semantic copy ----------
    Point p = {3, 4};
    Point q = p;                 // copy
    q.x = 10;                    // does not affect p
    print_int(p.x); putchar(' ');
    print_int(q.x); putchar('\n');   // 3 10

    // ---------- Named init {x = 1, y = 2}; names may appear in any order ----------
    Point r = {x = 1, y = 2};
    Point s = {y = 9, x = 8};
    print_int(r.y); putchar(' ');
    print_int(s.x); putchar('\n');   // 2 8

    // ---------- Zero init {T z = {}} ----------
    Point z = {};
    print_int(z.x); putchar(' ');
    print_int(z.y); putchar('\n');   // 0 0

    // ---------- Expression construction Point(...) / Point() ----------
    auto c = Point(5, 6);
    auto zero = Point();
    print_int(c.y); putchar(' ');    // 6
    print_int(zero.x); putchar('\n'); // 0

    // ---------- Returning a struct, assembled inside a function ----------
    auto made = make_point(7, 8);
    print_int(made.x); putchar(' ');
    print_int(made.y); putchar('\n');   // 7 8

    // ---------- Pass by value: changes inside the function do not propagate back ----------
    nudge(p);
    print_int(p.x); putchar('\n');   // still 3

    // ---------- Whole assignment = byte copy ----------
    Point m = {1, 1}, n = {2, 2};
    m = n;
    n.y = 99;                        // does not affect m
    print_int(m.y); putchar('\n');   // 2

    if p.x == 3 and q.x == 10 and r.x == 1 and s.y == 9 and made.y == 8 {
        puts("struct ok");
    }

    return 0;
}
