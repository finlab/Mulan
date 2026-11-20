unit match_const_var_pattern;
int f(int x) {
    int k = 3;
    match x {
        k: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(1); }

