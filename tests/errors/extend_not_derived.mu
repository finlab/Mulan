unit extend_not_derived;
interface I { int f(); }
interface J { int g() { return 1; } }
extend I with J;
int main() { return 0; }
