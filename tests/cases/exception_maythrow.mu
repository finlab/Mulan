unit exception_maythrow;

extern int printf(const char* fmt, ...);

class E : Exception {
    E(string m) { super(m); }
}

int leaf(int x) {
    return x + 1;
}

int fact(int n) {
    if n <= 1 { return 1; }
    return n * fact(n - 1);
}

int boom() {
    throw new E("boom");
}

int main() {
    int a = leaf(1);
    int b = fact(5);
    string s = "abc";
    int n = s.length() as int;
    try {
        boom();
    } catch (e: E) {
        printf("%d %d %d %d\n", a, b, n, e.message.length() as int);
    }
    return 0;
}
