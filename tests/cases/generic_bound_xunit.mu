// generic_bound_xunit — a generic function whose constraint names a type from another unit can be
// instantiated by a caller that does not import that unit: the bound is resolved where the template
// declares it, not at the call site (otherwise every caller of `std.mem.compare` would have to
// import `std.interfaces`).
unit generic_bound_xunit;

use constrain.order;
use std.cstdio;

int main() {
    int[] xs = new int[4];
    xs[0] = 7; xs[1] = 2; xs[2] = 9; xs[3] = 4;
    printf("%d\n", smallest<int>(xs));
    return 0;
}
