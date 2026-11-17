// if_cross_unit.mu — `#if` reads public compile-time constants of a `use`d unit (P3)
unit if_cross_unit;

use iflib.lib;

extern int puts(const char* s);

#if K == 7 {
    const int A = 1;
}
#else {
    const int A = 2;
}

#if iflib.lib.K == 7 {
    const int B = 1;
}
#else {
    const int B = 2;
}

#if ENABLED {
    const int C = 1;
}
#else {
    const int C = 2;
}

int main() {
    if A != 1 or B != 1 or C != 1 { puts("cross-unit condition failed"); return 1; }
    puts("cross unit ok");
    return 0;
}
