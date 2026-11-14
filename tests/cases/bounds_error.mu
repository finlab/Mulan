unit bounds_error;
use std.cstdio;
use std.exception;

// The compiler emits a bounds check for managed sequences (`T[]`); a violation throws
// `BoundsError` with the offending index, the axis length and the axis number.
// `--bounds=off` removes the check.
int main() {
    int[] s = new int[3];
    s[0] = 10;
    int caught = 0;
    try {
        s[3] = 1;
    } catch (e: BoundsError) {
        printf("1d index=%d length=%d axis=%d\n", e.index as int, e.length as int, e.axis as int);
        caught += 1;
    }

    int[,] m = new int[2,3];
    try {
        m[1,3] = 1;
    } catch (e: BoundsError) {
        printf("2d index=%d length=%d axis=%d\n", e.index as int, e.length as int, e.axis as int);
        caught += 1;
    }

    if caught != 2 { puts("bounds fail"); return 1; }
    puts("bounds ok");
    return 0;
}
