// decl_modifier_order.mu — declaration modifiers compose in any order (`nocopy` / `packed` /
// `align(N)`).
unit decl_modifier_order;

nocopy struct A { public const int x; A(int v) : x(v) { } }
packed struct B { public int x; }
align(8) struct C { public int x; }
nocopy packed struct D { public const int x; D(int v) : x(v) { } }
packed align(8) struct E { public int x; }

int main() {
    A a = A(1);
    D d = D(2);
    if a.x != 1 or d.x != 2 { return 1; }
    if sizeof(C) != 8 { return 2; }
    return 0;
}
