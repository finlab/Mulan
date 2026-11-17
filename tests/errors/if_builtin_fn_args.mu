unit if_builtin_fn_args;
use std.comptime;

#if std.comptime.is_windows(1) {
    extern int puts(const char* s);
}

int main() { return 0; }
