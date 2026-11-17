// if_builtin_fn.mu — a `#if` condition may call the `std.comptime` compile-time functions, also with
// the qualified `std.comptime.` form (SPEC 9.2 / 9.3).
unit if_builtin_fn;

use std.comptime;

extern int puts(const char* s);

#if std.comptime.is_windows() {
    const bool W = true;
}
#else {
    const bool W = false;
}

#if std.comptime.os == OS.Windows {
    const int WI = 1;
}
#else {
    const int WI = 0;
}

int main() {
    if W != is_windows() { puts("qualified builtin function mismatch"); return 1; }
    if WI != (os == OS.Windows ? 1 : 0) { puts("qualified builtin constant mismatch"); return 2; }
    puts("builtin fn ok");
    return 0;
}
