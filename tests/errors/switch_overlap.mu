unit switch_overlap;
int f(int x) {
    switch x {
        0..5: { return 1; }
        4: { return 2; }
        else: { return 0; }
    }
}
int main() { return f(1); }

