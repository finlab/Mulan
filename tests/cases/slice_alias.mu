unit slice_alias;
use std.cstdio;

// A dynamic array is a reference-counted handle: `b = a` copies the handle, so both names see the
// same buffer (SPEC 3.3). This pins the sharing semantics against a silent switch to deep copy.
int main() {
    int[] a = new int[3];
    a[0] = 1;
    int[] b = a;
    b[0] = 99;
    printf("a0=%d b0=%d\n", a[0], b[0]);
    if a[0] != 99 {
        return 1;
    }
    return 0;
}
