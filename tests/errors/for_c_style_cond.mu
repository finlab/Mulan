unit for_c_style_cond;

// The condition of a C-style `for` must be `bool`, exactly like `while`.

int main() {
    int n = 0;
    for (int i = 0; i < 3; i++) { n = n + i; }
    for (int j = 0; j; j++) { n = n + 1; }
    return n;
}
