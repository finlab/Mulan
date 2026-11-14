// generic_xunit.mu — a generic type declared in another unit, instantiated with a client-side type.
// `Box<Item>` here and `Box<genx.lib.Item>` inside the library must be two distinct instances: the
// instance name qualifies a type argument that comes from another unit, and the argument type
// itself is resolved at the use site (the template's unit cannot see it).
unit generic_xunit;

use genx.lib;

extern int puts(const char* s);

// Same name as the library's `Item`, but a different type.
struct Item {
    public int w;
}

int client_boxed(Box<Item> b) {
    return b.item.w;
}

int main() {
    Box<Item> x = { item = { w = 7 } };
    if client_boxed(x) != 7 { puts("client instance fail"); return 1; }

    Box<genx.lib.Item> y = genx.lib.lib_make(5);
    if genx.lib.lib_boxed(y) != 5 { puts("library instance fail"); return 2; }

    puts("generic xunit ok");
    return 0;
}
