unit audit_h1_this_ctor;
extern int puts(const char* s);
class Base { public int v; Base(int x) { v = x; } int get() const { return v; } }
const class Leaf : Base {
public:
    Leaf() : Base(1) { this.v = 5; }
}
int main() { Leaf l = new Leaf(); if l.get() != 5 { puts("ctor write via this"); return 1; } puts("ok"); return 0; }
