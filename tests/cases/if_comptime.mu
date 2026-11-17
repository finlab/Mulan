// if_comptime.mu — `use std.comptime;` makes the compiler-provided compile-time constants visible to
// `#if` as ordinary names (there is no separate "build constant" source).
unit if_comptime;

use std.comptime;

extern int puts(const char* s);

#if os == OS.Windows {
    const int A = 1;
}
#else {
    const int A = 2;
}

#if ptr_bits == 64 {
    const int B = 64;
}
#else {
    const int B = 32;
}

int main() {
    // Both arms compile; the selected value is one of the two, which proves the condition was
    // evaluated from the `comptime` constants rather than a hard-coded source.
    if A != 1 and A != 2 { puts("os condition failed"); return 1; }
    if B != 64 and B != 32 { puts("ptr_bits condition failed"); return 2; }
    puts("if comptime ok");
    return 0;
}
