unit string_index_bounds;
use std.cstdio;
use std.exception;

// SPEC 3.7: `s[i]` is bounds-checked, like `T[]`. A byte past the buffer is a heap read, so the
// library subscript throws `BoundsError` with the same index/length/axis fields.
int main() {
    string s = "abc";
    int caught = 0;
    try {
        byte b = s[100];
        printf("missed high\n");
    } catch (e: BoundsError) {
        if e.index == 100 and e.length == 3 {
            caught += 1;
        }
    }
    try {
        byte b = s[-1];
        printf("missed low\n");
    } catch (e: BoundsError) {
        if e.index == -1 and e.length == 3 {
            caught += 1;
        }
    }
    if s[1] != 'b' {
        return 1;
    }
    printf("caught=%d\n", caught);
    return 0;
}
