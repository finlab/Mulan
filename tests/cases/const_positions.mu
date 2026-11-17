// const_positions.mu — one compile-time evaluator feeds `step`, `align(N)` and switch ranges, so a
// unit-level `const` (and a `comptime` builtin) is accepted in all of them (SPEC 3.3.2).
unit const_positions;

use std.comptime;

extern int puts(const char* s);

const int STEP = 2;
const int AL = 16;
const int MID = 5;

align(AL) struct Aligned {
    public int a;
}

int main() {
    int sum = 0;
    for i in 0..10 step STEP {
        sum = sum + i;
    }
    if sum != 20 { puts("const step failed"); return 1; }

    int v = 3;
    int r = 0;
    switch v {
        0..MID: { r = 1; }
        else: { r = 2; }
    }
    if r != 1 { puts("const switch range failed"); return 2; }

    if sizeof(Aligned) != 16 { puts("const align failed"); return 3; }
    #if std.comptime.ptr_bits == 32 or std.comptime.ptr_bits == 64 {
        puts("const positions ok");
    }
    return 0;
}
