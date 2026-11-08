unit class_ptr;
class C { int v; C(){v=0;} ~C(){} }
int main() { C* p = alloc(8) as C*; return 0; }
