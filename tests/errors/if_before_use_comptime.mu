// A declaration-list `#if` sees only the `use`s written before it (SPEC 9.2, source-position rule):
// the build constants come from `use std.comptime`, which appears after this condition.
unit if_before_use_comptime;

#if os == OS.Windows {
    extern int puts(const char* s);
}

use std.comptime;

int main() { return 0; }
