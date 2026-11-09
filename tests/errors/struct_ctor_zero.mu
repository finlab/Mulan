unit struct_ctor_zero; struct S { int v; S(int x){v=x;} ~S(){} } int main(){ S a = 0; return 0; }
