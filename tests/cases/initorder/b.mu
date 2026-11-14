// initorder/b.mu — middle dependency of the unit_init_order case (SPEC 9.1)
public unit initorder.b;

use initorder.c;

extern int puts(const char* s);

struct BProbe {
    BProbe() { puts("init b"); }
    ~BProbe() { puts("fini b"); }
}

BProbe probe_b;
