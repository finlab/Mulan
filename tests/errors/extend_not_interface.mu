unit extend_not_interface;
struct P { public int f() { return 1; } }
interface I { int f(); }
interface J : I { int g() { return 1; } }
extend P with J;
int main() { return 0; }
