unit class_stress1;
extern int puts(const char* s);
interface I {
    int a();
    int b() { return a() + 1; }
    int c() { return b() + 1; }
}
class K : I {
    int seed;
    K(int s) { seed = s; }
    ~K() { }
    override int a() { return seed; }
}
class Holder {
    K k;
    Holder(K x) { k = x; }
    ~Holder() { del k; }
    int val() { return k.a(); }
}
int main() {
    K k = new K(10);
    if k.c() != 12 { puts("default chain fail"); return 1; }
    I i = k;
    if i.c() != 12 { puts("iface default chain fail"); return 1; }
    Holder h = new Holder(k);
    del k;
    if h.val() != 10 { puts("holder fail"); return 1; }
    del h;

    K outer = new K(1);
    {
        K inner = new K(2);
        if inner.a() != 2 { puts("inner fail"); return 1; }
    }
    if outer.a() != 1 { puts("shadow fail"); return 1; }

    I[2] arr = {};
    arr[0] = new K(5);
    arr[1] = new K(6);
    int sum = 0;
    for it in arr { sum += it.a(); }
    if sum != 11 { puts("for-in class array fail"); return 1; }
    del outer;
    puts("stress1 ok");
    return 0;
}

