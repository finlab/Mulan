unit override_mismatch;
class A { int f() { return 1; } }
class B : A { override int g() { return 2; } }
int main() { return 0; }
