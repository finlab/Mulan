unit struct_ctor_aggregate; struct S { int v; S(int x){v=x;} ~S(){} } int main(){ S a = {1}; return 0; }
