unit struct_dtor_field; struct S { int v; S(int x){v=x;} ~S(){} } struct O { S s; } int main(){ return 0; }
