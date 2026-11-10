unit e26; int f(int* p) { return *p; } int main() { int x = 1; const int* rp = &x; return f(rp); }
