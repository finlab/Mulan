unit switch_dup;
int f(int x) {
    switch x {
        1: { return 1; }
        1: { return 2; }
        else: { return 0; }
    }
}
int main() { return f(1); }

