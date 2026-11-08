unit iface_dup; interface I { int f(); } class C : I, I { override int f() { return 1; } } int main() { return 0; }
