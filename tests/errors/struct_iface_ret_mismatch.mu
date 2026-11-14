unit struct_iface_ret_mismatch;

interface I { int f(); }

struct S : I { override long f() { return 1; } }

int main() { return 0; }
