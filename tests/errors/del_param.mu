unit del_param;
class C { int v; C(){v=0;} ~C(){} }
void drop(C c) { del c; }
int main() { C x = new C(); drop(x); del x; return 0; }
