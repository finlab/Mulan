unit forin_ref_member_const;

// A member operator[] that returns `const ref T` is a read-only borrow: `for ref` needs a mutable one.
struct Vec {
    int[3] data;
    isize length() { return 3; }
    const ref int operator[](isize i) { return data[i]; }
}

int main() {
    Vec v = {};
    for ref x in v { }
    return 0;
}
