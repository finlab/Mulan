unit struct_ctor_nodefault; struct S { int v; S(int x){v=x;} ~S(){} } int main(){ S a; return 0; }
