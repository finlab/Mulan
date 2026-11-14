// struct_iface_default_xunit.mu — the default body is written in another unit (U34 M4): its type names
// resolve there (the private `Tag`), while S's own members stay reachable (the private `id`).
unit struct_iface_default_xunit;
use ifacedef.defs;

extern int puts(const char* s);

struct Box : ifacedef.defs.Named {
    int pad;

    private override int id() { return 41; }
}

int main() {
    Box b = {0};
    if (b.tagv() == 41 and b.offset() == 42) { puts("xunit default ok"); }
    return 0;
}
