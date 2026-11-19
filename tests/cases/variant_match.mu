// variant_match — `variant` declarations and `match` (SPEC 10.1.1/10.1.2): expression and statement
// forms, guards, `else`, `ref` bindings that mutate the payload, and `match move`.
unit variant_match;

extern int printf(const char* fmt, ...);

variant Value {
    Int int;
    Pair int, int;
    None;
}

int classify(Value v) {
    return match v {
        Int(n) if n > 10: 100;
        Int(n): n;
        Pair(a, b): a + b;
        None: -1;
    };
}

int main() {
    printf("%d %d %d %d\n", classify(Int(3)), classify(Int(20)), classify(Pair(4, 5)), classify(None));

    Value v = Int(7);
    match v {
        Int(ref x): { x = 9; }
        Pair(ref a, ref b): { a = 1; b = 2; }
        None: { }
    }
    printf("%d\n", match v { Int(n): n; Pair(a, b): a + b; None: -1; });

    Value m = Pair(3, 4);
    int q = match move m {
        Int(n): n;
        Pair(a, b): a * b;
        None: 0;
    };
    printf("%d\n", q);

    Value w = None;
    match w {
        Int(n): { printf("int\n"); }
        else: { printf("else\n"); }
    }
    return 0;
}
