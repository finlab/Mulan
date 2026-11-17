// A diamond re-visits its shared ancestors, so every interface walk keeps a visited set. The default of
// `I0` is inherited through both `A` and `B`: it is collected once, materializes once, and does not count
// as two conflicting defaults.
unit iface_diamond_default;

extern int puts(const char* s);

interface I0 { int f() { return 1; } }
interface A : I0 {}
interface B : I0 {}

struct S : A, B {}

int main() {
    S s;
    if s.f() != 1 { puts("diamond default failed"); return 1; }
    puts("iface diamond default ok");
    return 0;
}
