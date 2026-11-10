unit struct_dtor_global; struct S { int v; S(int x){v=x;} ~S(){} } S g = S(1); int main(){ return 0; }
