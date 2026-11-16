// A generic that is never instantiated is not checked; `--strict` reports it.
unit generic_uninstantiated;

T max<T>(T a, T b) { return a > b ? a : b; }

T unused<T>(T x) { return x.n; }

int main() { return max<int>(1, 2) - 2; }
