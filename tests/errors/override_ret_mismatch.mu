unit override_ret_mismatch; class A { virtual bool f() { return true; } } class B : A { override int f() { return 1; } } int main() { return 0; }
