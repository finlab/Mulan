unit forin_ref_member_const;

// A member operator[] that returns `ref const T` is a read-only borrow: `for ref` needs a mutable one.
struct Vec {
    int[3] data;
    usize length() { return 3; }
    ref const int operator[](usize i) { return data[i]; }
}

int main() {
    Vec v = {};
    for ref x in v { }
    return 0;
}
