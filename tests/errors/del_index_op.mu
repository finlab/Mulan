unit del_index_op;
class C { int x; C(){x=0;} ~C(){} }
class V { C c; V(C x){ c = x; } ~V(){} }
C operator[](V v, int i) { return v.c; }
int main() { C c = new C(); V v = new V(c); del v[0]; del v; del c; return 0; }
