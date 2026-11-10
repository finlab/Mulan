unit forin_ref;

// 用户类型不支持 for ref（operator[] 返回的是值）
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
