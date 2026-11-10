unit forin_nolen;

// 缺 length()：不能被 for-in 遍历
struct NoLen {
    int x;
}

int operator[](NoLen v, usize i) { return v.x; }

int main() {
    NoLen n = {};
    for x in n { }
    return 0;
}
