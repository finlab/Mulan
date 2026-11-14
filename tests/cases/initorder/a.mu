// initorder/a.mu — direct dependency of the unit_init_order case (SPEC 9.1)
public unit initorder.a;

use initorder.b;

extern int puts(const char* s);

struct AProbe {
    AProbe() { puts("init a"); }
    ~AProbe() { puts("fini a"); }
}

AProbe probe_a;
