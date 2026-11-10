unit switch_two_else;
int f(int x) {
    switch x {
        1: { return 1; }
        else: { return 0; }
        else: { return 2; }
    }
}
int main() { return f(1); }

