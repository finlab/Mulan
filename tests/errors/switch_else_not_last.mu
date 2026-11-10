unit switch_else_not_last;
int f(int x) {
    switch x {
        else: { return 0; }
        1: { return 1; }
    }
}
int main() { return f(1); }

