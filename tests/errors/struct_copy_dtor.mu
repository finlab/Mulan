unit struct_copy_dtor; struct S { int v; S(int x){v=x;} ~S(){} } int main(){ S a = S(1); S b = a; return 0; }
