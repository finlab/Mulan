// unit_init_dtor.mu — types that were previously rejected as globals now work (SPEC 9.1)
//
// `Tracked` has a user constructor and destructor; `Holder` has a reference field. Both used to be
// rejected with "no exit-time destruction".
unit unit_init_dtor;

extern int puts(const char* s);

class C {
    int v;
}

struct Holder {
    C c;
}

struct Tracked {
    int v;
    Tracked(int x) { v = x; puts("tracked ctor"); }
    ~Tracked() { puts("tracked dtor"); }
}

Holder holder = {};
Tracked tracked = Tracked(7);

int main() {
    if tracked.v != 7 { puts("bad ctor"); return 1; }
    if holder.c != null { puts("bad zero init"); return 1; }
    holder.c = new C();
    puts("main done");
    return 0;
}
