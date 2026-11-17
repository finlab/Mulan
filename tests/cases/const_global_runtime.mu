// const_global_runtime.mu — a `const` unit-level object may be initialized at run time (9.1)
unit const_global_runtime;

use std.string;

extern int puts(const char* s);

const string A = "ab";
const string B = A + "c";      // depends on A, initialized in declaration order
const int K = 1 + 2;           // still a compile-time constant

int main() {
    if B.length() != 3 { puts("runtime const string failed"); return 1; }
    if K != 3 { puts("compile-time const failed"); return 2; }
    puts("const global runtime ok");
    return 0;
}
