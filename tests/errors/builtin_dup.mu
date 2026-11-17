// SPEC 9.3: a builtin name may be declared once across the whole program.
unit builtin_dup;

use std.comptime;
use builtinlib.lib;

builtin const OS os;

int main() { return 0; }
