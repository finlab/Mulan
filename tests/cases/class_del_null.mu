unit class_del_null;
extern int puts(const char* s);
class C { int v; C(){v=0;} ~C(){} }
int main() { del null; C c = null; del c; puts("del null ok"); return 0; }

