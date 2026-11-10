unit switch_no_brace;
int f(int x) {
    switch x {
        1: return 1;
        else: return 0;
    }
}
int main() { return f(1); }

