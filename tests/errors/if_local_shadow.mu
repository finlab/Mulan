// A local variable may not appear in a `#if` condition (SPEC 9.2): a local that shares a builtin's name
// must not silently resolve to the builtin.
unit if_local_shadow;

use std.comptime;

int main() {
    int ptr_bits = 5;
#if ptr_bits == 64 {
    return 1;
}
#else {
    return 0;
}
}
