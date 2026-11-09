unit struct_dtor_return; struct S { int v; S(int x){v=x;} ~S(){} } S make(){ S a=S(1); return a; } int main(){ return 0; }
