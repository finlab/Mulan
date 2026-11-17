// SPEC 9.3: the declared type must be the type the compiler provides (`os` is an `OS`, not a `string`).
unit builtin_type_mismatch;

builtin const string os;

int main() { return 0; }
