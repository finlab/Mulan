unit if_live_use;
use std.comptime;

#if ptr_bits > 0 {
    use nosuch.thing;
}

int main() { return 0; }
