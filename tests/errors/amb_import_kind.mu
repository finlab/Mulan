unit amb_import_kind;

// `K` is a unit-level constant in `lib_amb_a` and an enum member in `lib_amb_b`: importing both
// units makes the bare name ambiguous, and it must not silently bind to one kind (#40).
use lib_amb_a;
use lib_amb_b;

int main() {
    int x = K;
    return x;
}
