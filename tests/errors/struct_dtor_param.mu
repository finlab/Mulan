unit struct_dtor_param; struct S { int v; S(int x){v=x;} ~S(){} } void f(S s){} int main(){ S a=S(1); f(a); return 0; }
