unit class_eqmix;
extern int puts(readonly char* s);
interface I1 { int f(); }
interface I2 : I1 { }
class C : I2 { int v; C(int x){v=x;} override int f(){return v;} }
int main() {
    C c = new C(1);
    I2 b = c;
    I1 a = b;
    if a != b { puts("iface eq fail"); return 1; }
    if a != c { puts("class-iface eq fail"); return 1; }
    del c; del b; del a;
    puts("eq mix ok");
    return 0;
}

