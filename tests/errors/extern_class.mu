unit extern_class;
class C { int v; C(){v=0;} ~C(){} }
extern void sink(C c);
int main() { return 0; }
