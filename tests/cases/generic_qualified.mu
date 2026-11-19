// generic_qualified — a call to a generic function can be qualified with its unit, exactly like an
// ordinary function. Two units exporting the same template make the unqualified name ambiguous, and
// the diagnostic that says "qualify it with a unit name" must name a spelling that resolves.
unit generic_qualified;

use qualgen.a;
use qualgen.b;
use std.cstdio;

int main() {
    printf("%d %d\n", qualgen.a.pick(0), qualgen.b.pick(0));          // inferred
    printf("%d %d\n", qualgen.a.pick<int>(0), qualgen.b.pick<int>(0));  // explicit
    return 0;
}
