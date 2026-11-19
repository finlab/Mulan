// generic_infer.mu — type arguments of a generic free function are inferred from the argument types
// (SPEC 3.15.3): a bare parameter, a pointer, a fixed array, a slice, a `ref` parameter, a nested
// generic (`Box<T>`, `Holder<T>`), a function/delegate type, a user type and a recursive instance.
// Explicit type arguments keep working and are never inferred.
unit generic_infer;

extern int puts(const char* s);
extern int printf(const char* fmt, ...);

interface IAdd {
    Self operator+(Self a, Self b);
}

interface IOrder {
    bool operator<(Self a, Self b);
}

struct Box<T> {
    T v;
}

class Holder<T> {
    public T v;
    public Holder(T x) { v = x; }
}

struct Money {
    long cents;
}

Money operator+(Money a, Money b) { return Money(a.cents + b.cents); }

T max2<T: IOrder>(T a, T b) { return a < b ? b : a; }

T sum2<T: IAdd>(T a, T b) { return a + b; }

T unbox<T>(Box<T> b) { return b.v; }

// A pattern nested twice: the instance is `Box<int>`, so `T` is `int`.
T inner<T>(Box<Box<T>> b) { return b.v.v; }

T held<T>(Holder<T> h) { return h.v; }

T deref<T>(T* p) { return *p; }

T first<T>(T[] s) { return s[0]; }

T firsta<T>(T[4] a) { return a[0]; }

// `ref T`: the pattern matches the referent, so the borrow is still checked as one.
T bump<T>(ref T x) {
    x = x + 1;
    return x;
}

// A value parameter: `const` is a view of the argument, so a `const int` argument still instantiates
// `T = int` and the body can write its own `T` local.
T bump_copy<T>(T x) {
    T y = x;
    y = y + 1;
    return y;
}

T apply<T>(delegate (T) => T f, T x) { return f(x); }

long mix<T, U>(T a, U b) { return (a as long) + (b as long); }

T sumto<T>(T n, T acc) {
    if n <= 0 { return acc; }
    return sumto(n - 1, acc + n);
}

// A constrained template whose body calls another template without type arguments: the body is also
// checked once at its definition (against a witness type), so a binding must never be baked into the
// template's own call node.
T top3<T: IOrder>(T a, T b, T c) { return max2(max2(a, b), c); }

int inc(int a) { return a + 1; }

int main() {
    int a = max2(1, 2);
    long b = max2(1L, 2L);            // a second instance of the same template
    int explicit_arg = max2<int>(3, 7);  // an explicit list is used as written

    Box<int> bi = {7};
    int c = unbox(bi);
    Box<Box<int>> bb = {bi};
    int d = inner(bb);

    Holder<long> h = new Holder<long>(9L);
    long e = held(h);

    int n = 11;
    int f = deref(&n);

    int[] xs = new int[3];
    xs[0] = 21;
    int g = first(xs);

    int[4] arr = {31, 32, 33, 34};
    int i = firsta(arr);
    int i2 = first(arr);  // a static array converts to the slice (T[N] -> T[])
    int i3 = deref(arr);  // ... and decays to the pointer to its first element

    int m = 5;
    int j = bump(m);

    const int ci = 41;
    int c1 = bump_copy(ci);  // the first instance is built from a const argument
    int c2 = bump_copy(7);   // ... and the same instance serves the later non-const one

    int k = apply((int q) => q + 2, 10);
    int l = apply(inc, 20);  // a non-capturing function value fits the delegate pattern

    Money pence = Money(2);
    Money pence2 = Money(3);
    Money o = sum2(pence, pence2);

    long p = mix(1, 2L);  // two type parameters, inferred independently
    int q = sumto(4, 0);  // the recursive call inside the body infers the same instance
    int r = top3(3, 7, 5);
    long s = top3(30L, 70L, 50L);  // a second instance of the enclosing template

    printf("max %d %ld %d\n", a, b, explicit_arg);
    printf("box %d %d\n", c, d);
    printf("class %ld\n", e);
    printf("ptr %d slice %d array %d decay %d %d\n", f, g, i, i2, i3);
    printf("ref %d %d\n", m, j);
    printf("fn %d %d\n", k, l);
    printf("user %ld mix %ld\n", o.cents, p);
    printf("rec %d\n", q);
    printf("nested %d %ld\n", r, s);
    printf("const %d %d\n", c1, c2);

    if (a == 2 and b == 2 and explicit_arg == 7 and c == 7 and d == 7 and e == 9 and
        f == 11 and g == 21 and i == 31 and i2 == 31 and i3 == 31 and m == 6 and j == 6 and
        k == 12 and l == 21 and c1 == 42 and c2 == 8 and
        o.cents == 5 and p == 3 and q == 10 and r == 7 and s == 70) {
        puts("generic infer ok");
    }
    return 0;
}
