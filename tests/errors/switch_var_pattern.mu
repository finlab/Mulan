unit switch_var_pattern;
int f(int x) {
    int k = 3;
    switch x {
        k: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(1); }

