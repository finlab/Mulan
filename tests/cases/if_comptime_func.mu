// if_comptime_func.mu — compiler-provided compile-time functions fold in `#if` and in ordinary code
unit if_comptime_func;

use std.comptime;

extern int puts(const char* s);

int main() {
    #if is_windows() {
        const bool IS_WIN = true;
    }
    #else {
        const bool IS_WIN = false;
    }
    if IS_WIN != is_windows() { puts("comptime function mismatch"); return 1; }
    puts("comptime func ok");
    return 0;
}
