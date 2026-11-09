unit struct_dtor_returnstmt; struct S { int v; S(){v=0;} ~S(){ return; } } int main(){ S a; return 0; }
