// generic_func.mu — free function monomorphization: explicit type arguments, multiple T, local variable type substitution
unit generic_func;

extern int puts(const char* s);

T max<T>(T a, T b) { return a > b ? a : b; }

T twice<T>(T x) {
    T r = x + x;
    return r;
}

int main() {
    int a = max<int>(3, 7);
    long b = max<long>(10, 4);
    int c = twice<int>(21);

    if (a == 7 and b == 10 and c == 42) { puts("generic func ok"); }
    return 0;
}
