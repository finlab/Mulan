// A guard does not cover its alternative: a failed guard falls through, so an unguarded arm (or
// `else`) is required — otherwise the expression form would have no value to produce.
unit match_guard_not_exhaustive;

variant V { A int; B; }

int f(V v) {
    return match v {
        A(x) if x > 0: 10;
        B: 20;
    };
}

int main() { return f(A(-1)); }
