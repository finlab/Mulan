unit iface_default_conflict; interface I1 { int f() { return 1; } } interface I2 { int f() { return 2; } } class C : I1, I2 { } int main() { return 0; }
