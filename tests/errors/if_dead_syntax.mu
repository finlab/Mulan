unit if_dead_syntax;
use std.comptime;

int main() {
#if os == OS.Windows and os == OS.Linux {
    int x = 1
}
    return 0;
}
