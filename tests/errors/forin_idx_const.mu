unit forin_idx_const;

// 索引变量 i 是只读的
int main() {
    int[3] a = {1, 2, 3};
    for i, x in a { i = 0; }
    return 0;
}
