// slice_agg_ctor_count — a slice aggregate constructs each element directly from its initializer,
// not by default-constructing it first (#239).
unit slice_agg_ctor_count;

extern int puts(const char* s);

int defs = 0;
int vals = 0;
struct S {
    public int v;
    S() { defs += 1; }
    S(int x) { v = x; vals += 1; }
}

int main() {
    S[] a = { S(1), S(2), S(3) };
    if defs != 0 or vals != 3 { puts("ctor counts"); return 1; }
    if a[0].v != 1 or a[2].v != 3 { puts("values"); return 2; }
    puts("slice agg ctor count ok");
    return 0;
}
