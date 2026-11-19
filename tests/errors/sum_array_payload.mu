// A payload stored by value must be copyable; a fixed-size array owns its elements, so `R[1]` is
// rejected exactly like `R` (#92).
unit sum_array_payload;

struct R { int x; ~R() {} }

variant S {
    Values R[1];
    None;
}

int main() { return 0; }
