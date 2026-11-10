unit class_iface_return;
extern int puts(const char* s);
int live = 0;
interface I { I clone(); int f(); bool same(I x) { return this == x; } }
class C : I {
    int v;
    C(int x){v=x;live+=1;}
    ~C(){live-=1;}
    override I clone() { return new C(v); }
    override int f() { return v; }
}
int main() {
    C c = new C(4);
    I i = c;
    I j = i.clone();
    if j.f() != 4 { puts("iface ret fail"); return 1; }
    if not i.same(c) { puts("iface this compare fail"); return 1; }
    j = null;
    c = null;
    i = null;
    if live != 0 { puts("iface ret live fail"); return 1; }
    puts("iface ret ok");
    return 0;
}

