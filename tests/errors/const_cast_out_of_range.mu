// A constant `as` conversion whose value does not fit the target type is a compile error, matching the
// run-time check (SPEC 8.2).
unit const_cast_out_of_range;

const int C = 1.0e30 as int;

int main() { return C; }
