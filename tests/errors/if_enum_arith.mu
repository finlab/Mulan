// `#if` follows the language's enum rules: two members of the same enum compare, but arithmetic on them
// needs an explicit conversion. Folding the member indices silently accepted `OS.Windows + OS.Linux`
// and could select the wrong platform arm (#73).
unit if_enum_arith;

use std.comptime;

#if OS.Windows + OS.Linux == 1 {
int main() { return 0; }
}
#else {
int main() { return 0; }
}
