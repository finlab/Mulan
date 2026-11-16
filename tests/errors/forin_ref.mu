unit forin_ref;

// user types do not support for ref (operator[] returns a value)
struct Vec {
    int[4] data;
    isize n;
    isize length() { return n; }
}

int operator[](Vec v, isize i) { return v.data[i]; }

int main() {
    Vec v = {};
    for ref x in v { }
    return 0;
}
