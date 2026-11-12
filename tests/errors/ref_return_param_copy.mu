unit rr2;

struct Small {
    int[4] v;
}

// 按值接收者：返回的借用指向参数副本（内联存储），会悬垂
ref int bad(Small s, usize i) {
    return s.v[i];
}

int main() {
    return 0;
}
