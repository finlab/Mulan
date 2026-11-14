unit class_iface_array;
extern int puts(const char* s);
int live = 0;
interface I { int f(); }
class C : I { int v; C(int x){v=x;live+=1;} ~C(){live-=1;} override int f(){return v;} }
class Holder { I[2] items; Holder(){ items[0]=null; items[1]=null; } ~Holder(){} }
I[2] make2() {
    I[2] t = {};
    t[0] = new C(1);
    t[1] = new C(2);
    return t;
}
int main() {
    I[2] r = make2();
    if r[0].f() + r[1].f() != 3 { puts("iface array return fail"); return 1; }
    if live != 2 { puts("iface array return rc fail"); return 1; }
    Holder h = new Holder();
    for i in 0..2 { h.items[i] = r[i]; }   // element-wise: array fields are not assignable (C99)
    h = null;
    if r[0].f() != 1 { puts("iface array field rc fail"); return 1; }
    if live != 2 { puts("iface array field live fail"); return 1; }
    puts("iface array ok");
    return 0;
}

