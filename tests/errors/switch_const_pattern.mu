unit switch_const_pattern;
const int K = 3;
int f(int x) {
    switch x {
        K: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(3); }

