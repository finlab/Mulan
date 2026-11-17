// decl_modifier_order.mu — declaration modifiers compose in any order (`const` / `nocopy` / `packed` /
// `align(N)`); `const` is a declaration modifier when a type declaration follows, whatever the order.
unit decl_modifier_order;

const nocopy struct A { public const int x; A(int v) : x(v) { } }
const packed struct B { public int x; }
const align(8) struct C { public int x; }
nocopy const struct D { public const int x; D(int v) : x(v) { } }
packed align(8) struct E { public int x; }
const struct F { public int x; }

int main() {
    A a = A(1);
    D d = D(2);
    if a.x != 1 or d.x != 2 { return 1; }
    if sizeof(C) != 8 { return 2; }
    return 0;
}
