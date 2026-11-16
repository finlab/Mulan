unit struct_dtor_array; struct S { int v; ~S(){} } struct O { S[2] a; } int main(){ O x; O y = x; return 0; }
