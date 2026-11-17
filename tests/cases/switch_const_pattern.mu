// switch_const_pattern.mu — a unit-level compile-time constant is a valid switch pattern, like a literal
// or an enum member (SPEC 3.3.2: `#if`, array lengths, `step`, `align(N)` and switch ranges share one
// compile-time evaluator).
unit switch_const_pattern;

extern int puts(const char* s);

const int K = 3;
const int LO = 10;
const int HI = 20;

int f(int x) {
    switch x {
        K: { return 1; }
        LO..HI: { return 2; }
        else: { return 0; }
    }
}

int main() {
    if f(3) != 1 { puts("const pattern failed"); return 1; }
    if f(10) != 2 or f(15) != 2 or f(19) != 2 { puts("const range failed"); return 2; }
    if f(20) != 0 or f(4) != 0 { puts("const range leaked"); return 3; }
    puts("const pattern ok");
    return 0;
}
