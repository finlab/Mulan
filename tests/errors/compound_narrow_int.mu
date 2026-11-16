unit compound_narrow_int;
// SPEC 3.6: no implicit narrowing. `i += L` is `i = i + L`, whose result is `long`.
int main() {
    long L = 1099511627776;
    int i = 0;
    i += L;
    return 0;
}
