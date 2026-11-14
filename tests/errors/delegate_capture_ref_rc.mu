unit delegate_capture_ref_rc;
use std.string;

type Get = delegate () => int;

int main() {
    String s = "hi";
    // a borrow cannot hold a reference-counted value: capture by value instead
    Get g = [ref s] () => s.length() as int;
    return g();
}
