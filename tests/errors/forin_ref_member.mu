unit forin_ref_member;

// A member operator[] that returns a value does not support `for ref` either (issue #27): the
// member overload takes precedence in resolve_index_shape, so the borrow check has to cover it.
struct Vec {
    int[3] data;
    isize length() { return 3; }
    int operator[](isize i) { return data[i]; }
}

int main() {
    Vec v = {};
    for ref x in v { }
    return 0;
}
