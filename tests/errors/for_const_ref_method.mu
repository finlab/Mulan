unit for_const_ref_method;
struct S { int v; void poke() { v = 1; } }
int main() { S[2] a; S[] arr = a; for const ref x in arr { x.poke(); } return 0; }
