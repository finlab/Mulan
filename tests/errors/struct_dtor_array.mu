unit struct_dtor_array; struct S { int v; S(int x){v=x;} ~S(){} } int main(){ S[2] a = 0; return 0; }
