unit type_alias_cross;

use lib_alias;

int main() {
    Handle h = 1;
    PAlias p = {2, 3};
    lib_alias.Handle h2 = 4;
    return h + h2 + p.a + p.b;
}
