unit for_const_ref_field;
struct S { int v; }
int main() { S[2] a; S[] arr = a; for const ref x in arr { x.v = 42; } return 0; }
