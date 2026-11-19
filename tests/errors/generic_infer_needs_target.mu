unit generic_infer_needs_target;

// An argument that needs its parameter type to be meaningful (`Some(3)` before the sum type is known)
// cannot be typed during inference: the diagnostic keeps the original reason and names the way out
// (SPEC 3.15.3).

use std.cstdio;

type Maybe = Some(int) | None;

T unwrap<T>(Maybe m) {
    return 0;
}

int main() {
    int a = unwrap(Some(3));
    printf("%d\n", a);
    return 0;
}
