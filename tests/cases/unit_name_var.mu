unit unit_name_var;

// when a local/global var shares the unit name, `v.field = x` must still be a valid lvalue
// (is_lvalue must not treat it as unit-qualified access just because the name equals the unit name).

extern int puts(const char* s);

struct S { int x; }

S unit_name_var = {1};

int main() {
    unit_name_var.x = 7;
    if unit_name_var.x != 7 { puts("global field assign failed"); return 1; }

    S unit_name_var = {2};  // local shadows the same-named global
    unit_name_var.x = 9;
    if unit_name_var.x != 9 { puts("local field assign failed"); return 2; }

    puts("unit name var ok");
    return 0;
}
