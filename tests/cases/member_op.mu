// member_op.mu — member operators on a struct (0.0.87): borrowed receiver, no copy and no RC on `s[i]`
unit member_op;

extern int putchar(int c);

void print_int(int v) {
    if v == 0 { putchar('0'); return; }
    if v < 0 { putchar('-'); v = -v; }
    int[20] digits;
    int i = 0;
    while v > 0 { digits[i] = v % 10; v /= 10; i += 1; }
    while i > 0 { i -= 1; putchar('0' + digits[i]); }
}

struct Vec2 {
    int x;
    int y;

    int operator[](usize i) {
        if i == 0 { return x; }
        return y;
    }

    Vec2 operator+(Vec2 o) {
        Vec2 r = {x + o.x, y + o.y};
        return r;
    }

    bool operator==(Vec2 o) { return x == o.x and y == o.y; }

    int operator-() { return -(x + y); }
}

// index-form for-in through a member operator[] (borrowed receiver: no copy per iteration)
struct Bag {
    int[4] data;
    usize n;
    usize length() { return n; }
    int operator[](usize i) { return data[i]; }
}

// write form: `[]=` is a member too
struct Acc {
    int total;
    int operator[](usize i) { return total; }
    void operator[]=(usize i, int v) { total += v; }
}

Vec2 make(int x, int y) { Vec2 v = {x, y}; return v; }

int main() {
    Vec2 a = {1, 2};
    Vec2 b = {3, 4};
    print_int(a[0]); putchar(' '); print_int(a[1]); putchar('\n');  // 1 2

    Vec2 c = a + b;
    print_int(c.x); putchar(' '); print_int(c.y); putchar('\n');    // 4 6

    if (a == a) { putchar('E'); }
    if (a != b) { putchar('N'); }                                   // EN (from member operator==)
    putchar('\n');

    print_int(-a); putchar('\n');                                   // -3
    print_int(make(7, 8)[1]); putchar('\n');                        // 8 (temporary receiver)

    Bag g = {};
    g.data[0] = 5; g.data[1] = 6; g.data[2] = 7; g.n = 3;
    int sum = 0;
    for v in g { sum += v; }
    print_int(sum); putchar('\n');                                  // 18
    for i, v in g { sum += v; }
    print_int(sum); putchar('\n');                                  // 36

    Acc acc = {};
    acc[0] = 5;
    acc[1] = 7;
    print_int(acc[0]); putchar('\n');                               // 12
    return 0;
}
