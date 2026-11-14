// export_abi_mutual — internal functions and `export`ed functions call each other, across units and
// within one: the calling convention belongs to the symbol, not to the caller, so both directions
// are ordinary calls (0.0.121).
unit export_abi_mutual;

use std.cstdio;
use exportabi.api;

int main() {
    Pair p = { a = 3, b = 4 };

    // from another unit, into an exported function (C ABI: byval / sret)
    printf("export %d\n", take(p) as int);
    Pair q = make(7, 8);
    printf("export back %d %d\n", q.a as int, q.b as int);

    // and from an ordinary function of the library into its own exported functions
    printf("internal->export %d\n", via_export(p) as int);
    Pair r = via_make(7, 8);
    printf("internal->export back %d %d\n", r.a as int, r.b as int);
    return 0;
}
