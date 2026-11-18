unit audit_h1_dtor_this;
extern int puts(const char* s);
int seen = 0;
class Base { public int v; Base(int x) { v = x; } int get() const { return v; } }
const class Leaf : Base {
public:
    Leaf() : Base(1) { }
    ~Leaf() { this.v = 9; seen = get(); }
}
int main() { { Leaf l = new Leaf(); } if seen != 9 { puts("dtor this write"); return 1; } puts("ok"); return 0; }
