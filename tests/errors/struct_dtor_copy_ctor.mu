// struct_dtor_copy_ctor — a host struct must not be copied by value just because its dtor-bearing field
// declares a copy constructor: the compiler's copy glue reproduces handles, it does not invoke the field's
// constructor, so a byte copy would alias the owned block and free it twice (agents/value-semantics.md §3).
unit struct_dtor_copy_ctor;
struct S {
    byte* p;
    S() { p = null; }
    S(const ref S o) { p = null; }
    ~S() { if (p != null) { free(p); } }
}
struct O { S s; int tag; }
int main() {
    O a = {};
    O b = a;
    return 0;
}
