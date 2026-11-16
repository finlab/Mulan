unit struct_dtor_field; struct S { int v; ~S(){} } struct O { S s; } int main(){ O a; O b = a; return b.s.v; }
