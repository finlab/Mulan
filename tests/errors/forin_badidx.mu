unit forin_badidx;

// the index of operator[] must be usize (written as int here, no narrowing conversion)
struct Vec {
    int[4] data;
    usize n;
    usize length() { return n; }
}

int operator[](Vec v, int i) { return v.data[i]; }

int main() {
    Vec v = {};
    for x in v { }
    return 0;
}
