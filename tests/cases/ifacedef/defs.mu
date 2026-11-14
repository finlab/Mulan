// Helper unit for struct_iface_default_xunit: the default body names a unit-private struct, so a
// struct in another unit can only be satisfied by resolving the body in this unit (U34 M4).
public unit ifacedef.defs;

struct Tag { int v; }

public interface Named {
    int id();
    int tagv() { Tag t = { this.id() }; return t.v; }
    int offset() { return this.tagv() + 1; }
}
