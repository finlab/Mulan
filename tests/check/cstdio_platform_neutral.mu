unit cstdio_platform_neutral;

use std.cstdio;

// #39: `std.cstdio` is the portable declaration layer and must not export the
// implementation-defined `BUFSIZ` / `FILENAME_MAX`; the platform values live in `cstd.stdio`.
// The fixed C constants stay available.
int main() {
    if EOF != -1 or SEEK_SET != 0 or SEEK_CUR != 1 or SEEK_END != 2 {
        return 1;
    }
    return 0;
}
