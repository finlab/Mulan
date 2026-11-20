// A payload stored by value must be movable; `nomove` marks a value whose address is part of it,
// so it cannot be transferred into a variant (the D10 rule replaced the old copyability rule).
unit sum_array_payload;

nomove struct R { int x; ~R() {} }

variant S {
    Values R[1];
    None;
}

int main() { return 0; }
