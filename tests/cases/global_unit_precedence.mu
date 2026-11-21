// global_unit_precedence — a global variable takes precedence over a same-named unit in member
// access (#196).
unit global_unit_precedence;

use globunit.t;

extern int puts(const char* s);

struct S { public int c; }
S t;

int main() {
    t.c = 42;
    if t.c != 42 { puts("field"); return 1; }
    puts("global unit precedence ok");
    return 0;
}
