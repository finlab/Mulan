// builtinlib.lib — declares a builtin name that `std.comptime` also declares: a program-wide duplicate
// (SPEC 9.3), used by the `builtin_dup` must-fail case.
public unit builtinlib.lib;

use std.comptime;

public builtin const OS os;
