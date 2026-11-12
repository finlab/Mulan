unit forin_badlen;

// length() must return usize
struct Vec {
    int[4] data;
    int n;
    int length() { return n; }
}

int operator[](Vec v, usize i) { return v.data[i]; }

int main() {
    Vec v = {};
    for x in v { }
    return 0;
}
