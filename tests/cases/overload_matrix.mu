// overload_matrix — ref-feasibility in ranking (#190), named-constructor selection (#191), a delegate
// target for overloads (#192), and const overloads on a mutable receiver (#194).
unit overload_matrix;

extern int puts(const char* s);

int f(ref int x) { return 1; }
int f(long x) { return 2; }

struct P {
    public int which;
    P(int x) { which = 1; }
    P(long y) { which = 2; }
}

int g(int x) { return x; }
float g(float x) { return x; }
type D = delegate (int) => int;

class C {
    public int f(int x) const { return 10; }
    public int f(long x) { return 20; }
}

int main() {
    const int k = 5;
    if f(k) != 2 { puts("ref feasibility"); return 1; }

    P p = P(y = 5);
    if p.which != 2 { puts("named ctor"); return 2; }

    D d = g;
    if d(3) != 3 { puts("delegate target"); return 3; }

    C c = new C();
    if c.f(5) != 10 { puts("const overload"); return 4; }

    puts("overload matrix ok");
    return 0;
}
