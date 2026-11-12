unit forin_idx_const;

// the index variable i is read-only
int main() {
    int[3] a = {1, 2, 3};
    for i, x in a { i = 0; }
    return 0;
}
