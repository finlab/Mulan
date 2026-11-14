// initorder/c.mu — deepest dependency of the unit_init_order case (SPEC 9.1)
public unit initorder.c;

extern int puts(const char* s);

struct CProbe {
    CProbe() { puts("init c"); }
    ~CProbe() { puts("fini c"); }
}

CProbe probe_c;
