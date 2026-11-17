// const_struct_as_removal.mu — `as` is the explicit, unsafe escape hatch, including `const` removal
// (SPEC 3.7: there is no `const_cast`). A value struct copies, so the write touches the copy only.
unit const_struct_as_removal;

extern int puts(const char* s);

struct P {
    public int x;
    P(int v) : x(v) { }
}

int main() {
    const P c = P(1);
    P m = c as P;
    m.x = 9;
    if m.x != 9 { puts("as-removal did not produce a writable copy"); return 1; }
    if c.x != 1 { puts("the const original was modified"); return 2; }
    puts("const as removal ok");
    return 0;
}
