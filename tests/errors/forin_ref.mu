unit forin_ref;

// user types do not support for ref (operator[] returns a value)
struct Vec {
    int[4] data;
    usize n;
    usize length() { return n; }
}

int operator[](Vec v, usize i) { return v.data[i]; }

int main() {
    Vec v = {};
    for ref x in v { }
    return 0;
}
