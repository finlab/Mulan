unit dyn_dtor_elem;
struct S { int v; ~S() {} }
int main() { S[] s = new S[2]; return 0; }
