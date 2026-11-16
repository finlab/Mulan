unit forin_badidx;

// the index of operator[] must be isize (written as int here, no narrowing conversion)
struct Vec {
    int[4] data;
    isize n;
    isize length() { return n; }
    int operator[](int i) { return data[i]; }
}

int main() {
    Vec v = {};
    for x in v { }
    return 0;
}
