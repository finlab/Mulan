unit forin_badidx;

// operator[] 的下标必须是 usize（这里写成 int，不能窄化转换）
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
