// bounds_off.mu — with `--bounds=off` the compiler removes the subscript check
//
// `bounds_error.mu` documents that `--bounds=off` removes the check, but nothing
// verified it. This case is built with `--bounds=off` (see the `@ --bounds=off`
// marker in the expected file).
unit bounds_off;

use std.cstdio;
use std.exception;

int main() {
    // ---- in-bounds access keeps working ----
    int[,] m = new int[2,3];
    m[0,0] = 0;
    m[0,1] = 1;
    m[0,2] = 2;
    m[1,0] = 3;
    m[1,1] = 4;
    m[1,2] = 5;
    printf("in-bounds=%d %d\n", m[1,2], m[0,0]);

    // ---- the out-of-bounds subscript no longer throws ----
    // m[0,4] is out of bounds on axis 1 (the axis length is 3), but the linear index
    // 4 is inside the allocation: the read returns the element m[1,1] == 4. The value
    // is therefore well defined -- no unspecified memory is touched.
    int caught = 0;
    int v = 0;
    try {
        v = m[0,4];
    } catch (e: BoundsError) {
        caught += 1;
    }
    if caught != 0 {
        puts("bounds check still active");
        return 1;
    }
    printf("out-of-bounds=%d\n", v);

    puts("bounds off ok");
    return 0;
}
