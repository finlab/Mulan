unit forin_badlen;

// length() must return isize
struct Vec {
    int[4] data;
    int n;
    int length() { return n; }
}

int operator[](Vec v, isize i) { return v.data[i]; }

int main() {
    Vec v = {};
    for x in v { }
    return 0;
}
