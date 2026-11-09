unit overload_ambiguous; int f(int a) { return 1; } int f(long a) { return 2; } int main() { byte b = 1; int x = f(b); return x; }

