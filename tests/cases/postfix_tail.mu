unit postfix_tail;
extern int puts(const char* s);
class C { int v; C(int x){v=x;} ~C(){} int get(){return v;} }
int main() {
    int n = len("abc") as int;
    if n != 3 { puts("len as fail"); return 1; }
    int x = new C(4).get();
    if x != 4 { puts("new dot fail"); return 1; }
    int m = sizeof(C) as int;
    if m != 8 { puts("sizeof fail"); return 1; }
    puts("postfix tail ok");
    return 0;
}

