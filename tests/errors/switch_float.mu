unit switch_float;
int f(double x) {
    switch x {
        1: { return 1; }
        else: { return 0; }
    }
}
int main() { return f(1.0); }

