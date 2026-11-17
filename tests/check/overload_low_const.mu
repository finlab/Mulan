// overload_low_const.mu — low-level `const` is part of the signature: `g(int*)` and `g(const int*)`
// are two overloads (only a *top-level* const is folded away, SPEC 3.7).
unit overload_low_const;

const int CV = 7;

int g(int* p) { return 1; }
int g(const int* p) { return 2; }

int main() {
    int v = 3;
    if g(&v) != 1 { return 1; }
    if g(&CV) != 2 { return 2; }
    return 0;
}
