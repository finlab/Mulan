// implicit_ctor.mu — `implicit T(one_param)` (0.0.88): conversion at init / argument / return / assignment,
// weaker than every built-in conversion in overload resolution, and usable on an RC-bearing struct
unit implicit_ctor;

extern int putchar(int c);

void print_int(int v) {
    if v == 0 { putchar('0'); return; }
    if v < 0 { putchar('-'); v = -v; }
    int[20] digits;
    int i = 0;
    while v > 0 { digits[i] = v % 10; v /= 10; i += 1; }
    while i > 0 { i -= 1; putchar('0' + digits[i]); }
}

struct Wrap {
    int v;
    implicit Wrap(int x) { v = x; }
}

struct Buf {
    byte[] b;
    implicit Buf(int n) {
        b = new byte[n];
        b[0] = 7;
    }
}

int take(Wrap w) { return w.v; }
int take_int(int x) { return x + 100; }   // exact `int` overload must win over the conversion

Wrap give() { return 7; }

int main() {
    Wrap a = 3;                             // initialization
    print_int(a.v); putchar('\n');          // 3

    print_int(take(5)); putchar('\n');      // 5 (argument)
    print_int(give().v); putchar('\n');     // 7 (return)
    print_int(take_int(5)); putchar('\n');  // 105 (exact match wins)

    Wrap b = 1;
    b = 9;                                  // assignment
    print_int(b.v); putchar('\n');          // 9

    Buf buf = 4;                            // RC-bearing struct
    print_int(buf.b[0] as int); putchar('\n');  // 7
    return 0;
}
