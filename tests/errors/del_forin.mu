unit del_forin;
class C { int v; C(){v=0;} ~C(){} }
int main() { C[1] a = 0; a[0] = new C(); for c in a { del c; } return 0; }
