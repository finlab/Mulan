// shadow_platform — a library ships one implementation per OS under `platform/<os>/`; the loader
// picks the matching one before the portable fallback. Both accepted file forms appear across the
// three platforms (flat `shadowlib.probe.mu` and slashed `shadowlib/probe.mu`), so a CI matrix
// exercises both. The portable `shadowlib/probe.mu` returns 0 and must never be selected.

unit shadow_platform;

use std.cstdio;
use shadowlib.probe;

int main() {
    printf("%d\n", which());
    return 0;
}
