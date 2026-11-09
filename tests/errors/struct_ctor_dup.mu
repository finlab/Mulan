unit struct_ctor_dup; struct S { int v; S(int a){v=a;} S(int b){v=b;} ~S(){} } int main(){ return 0; }
